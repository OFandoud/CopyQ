/*
    Copyright (c) 2012, Lukas Holecek <hluk@email.cz>

    This file is part of CopyQ.

    CopyQ is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    CopyQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with CopyQ.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "clipboardserver.h"
#include "clipboardmonitor.h"
#include "mainwindow.h"
#include "clipboardbrowser.h"
#include "clipboarditem.h"
#include "arguments.h"

#include <QLocalSocket>
#include <QMimeData>
#include <QTimer>

#ifdef NO_GLOBAL_SHORTCUTS
struct QxtGlobalShortcut {};
#else
#include "../qxt/qxtglobalshortcut.h"
#endif

ClipboardServer::ClipboardServer(int &argc, char **argv)
    : App(argc, argv)
    , m_server(NULL)
    , m_monitorserver(NULL)
    , m_socket(NULL)
    , m_wnd(NULL)
    , m_monitor(NULL)
    , m_lastHash(0)
    , m_shortcutActions()
{
    // listen
    m_server = newServer( serverName(), this );
    if ( !m_server->isListening() )
        return;

    // don't exit when all windows closed
    QApplication::setQuitOnLastWindowClosed(false);

    // main window
    m_wnd = new MainWindow;

    // listen
    m_monitorserver = newServer( monitorServerName(), this );
    connect( m_server, SIGNAL(newConnection()),
             this, SLOT(newConnection()) );
    connect( m_monitorserver, SIGNAL(newConnection()),
             this, SLOT(newMonitorConnection()) );

    connect( m_wnd, SIGNAL(destroyed()),
             this, SLOT(quit()) );
    connect( m_wnd, SIGNAL(changeClipboard(const ClipboardItem*)),
             this, SLOT(changeClipboard(const ClipboardItem*)));

    loadSettings();

    // notify window if configuration changes
    ConfigurationManager *cm = ConfigurationManager::instance();
    connect( cm, SIGNAL(configurationChanged()),
             this, SLOT(loadSettings()) );

    // hash of the last clipboard data
    bool ok;
    m_lastHash = cm->value("_last_hash").toUInt(&ok);
    if (!ok)
        m_lastHash = 0;

    // run clipboard monitor
    startMonitoring();
}

ClipboardServer::~ClipboardServer()
{
    if( isMonitoring() )
        stopMonitoring();

    if (m_socket)
        m_socket->disconnectFromServer();

    delete m_wnd;

    // delete shortcuts manually
    foreach (QxtGlobalShortcut *s, m_shortcutActions.keys())
        delete s;
    m_shortcutActions.clear();
}

void ClipboardServer::monitorStateChanged(QProcess::ProcessState newState)
{
    if (newState == QProcess::NotRunning) {
        monitorStandardError();

        QString msg = tr("Clipboard monitor crashed!");
        log(msg, LogError);
        m_wnd->showError(msg);

        // restart clipboard monitor
        stopMonitoring();
        startMonitoring();
    } else if (newState == QProcess::Starting) {
        log( tr("Clipboard Monitor: Starting") );
    } else if (newState == QProcess::Running) {
        log( tr("Clipboard Monitor: Started") );
    }
}

void ClipboardServer::monitorStandardError()
{
    log( tr("Clipboard Monitor: ") +
            m_monitor->readAllStandardError(), LogError );
}

void ClipboardServer::stopMonitoring()
{
    ConfigurationManager *cm = ConfigurationManager::instance();
    cm->setValue("_last_hash", m_lastHash);

    if (m_monitor) {
        m_monitor->disconnect( SIGNAL(stateChanged(QProcess::ProcessState)) );

        if ( m_monitor->state() != QProcess::NotRunning ) {
            log( tr("Clipboard Monitor: Terminating") );

            if (m_socket) {
                m_socket->disconnectFromServer();
                m_monitor->waitForFinished(1000);
            }

            if ( m_monitor->state() != QProcess::NotRunning ) {
                log( tr("Clipboard Monitor: Command 'exit' unsucessful!"),
                     LogError );
                m_monitor->terminate();
                m_monitor->waitForFinished(1000);

                if ( m_monitor->state() != QProcess::NotRunning ) {
                    log( tr("Clipboard Monitor: Cannot terminate process!"),
                         LogError );
                    m_monitor->kill();

                    if ( m_monitor->state() != QProcess::NotRunning ) {
                        log( tr("Clipboard Monitor: Cannot kill process!!!"),
                             LogError );
                    }
                }
            }
        }

        if ( m_monitor->state() == QProcess::NotRunning ) {
            log( tr("Clipboard Monitor: Terminated") );
        }

        m_socket->deleteLater();
        m_socket = NULL;
        m_monitor->deleteLater();
        m_monitor = NULL;
    }
}

void ClipboardServer::startMonitoring()
{
    if ( !m_monitor ) {
        m_monitor = new QProcess(this);
        connect( m_monitor, SIGNAL(stateChanged(QProcess::ProcessState)),
                 this, SLOT(monitorStateChanged(QProcess::ProcessState)) );
        connect( m_monitor, SIGNAL(readyReadStandardError()),
                 this, SLOT(monitorStandardError()) );
        m_monitor->start( QApplication::arguments().at(0),
                          QStringList() << "monitor",
                          QProcess::ReadOnly );
        if ( !m_monitor->waitForStarted(2000) ) {
            log( tr("Cannot start clipboard monitor!"), LogError );
            m_monitor->deleteLater();
            exit(10);
            return;
        }
    }
    m_wnd->browser(0)->setAutoUpdate(true);
}

void ClipboardServer::loadMonitorSettings()
{
    if ( !m_socket || !m_socket->isWritable() )
        return;

    ConfigurationManager *cm = ConfigurationManager::instance();

    QVariantMap settings;
    settings["_last_hash"] = cm->value("_last_hash");
    settings["formats"] = cm->value("formats");
    settings["check_clipboard"] = cm->value("check_clipboard");
#ifdef Q_WS_X11
    settings["copy_clipboard"] = cm->value("copy_clipboard");
    settings["copy_selection"] = cm->value("copy_selection");
    settings["check_selection"] = cm->value("check_selection");
#endif

    QByteArray settings_data;
    QDataStream settings_out(&settings_data, QIODevice::WriteOnly);
    settings_out << settings;

    ClipboardItem item;
    item.setData("application/x-copyq-settings", settings_data);

    QByteArray msg;
    QDataStream out(&msg, QIODevice::WriteOnly);
    out << item;
    writeMessage(m_socket, msg);
}

void ClipboardServer::newConnection()
{
    QLocalSocket* client = m_server->nextPendingConnection();
    connect(client, SIGNAL(disconnected()),
            client, SLOT(deleteLater()));

    Arguments args;
    QByteArray msg;
    readMessage(client, &msg);
    QDataStream in(msg);
    in >> args;

    QByteArray client_msg;
    // try to handle command
    int exitCode = doCommand(args, &client_msg);
    if ( exitCode == CommandBadSyntax ) {
        client_msg = tr("Bad command syntax. Use -h for help.\n").toLocal8Bit();
    }
    sendMessage(client, client_msg, exitCode);

    client->disconnectFromServer();
}

void ClipboardServer::sendMessage(QLocalSocket* client, const QByteArray &message, int exitCode)
{
    QByteArray msg;
    QDataStream out(&msg, QIODevice::WriteOnly);
    out << exitCode;
    out.writeRawData( message.constData(), message.length() );
    writeMessage(client, msg);
}

void ClipboardServer::newMonitorConnection()
{
    if (m_socket) {
        m_socket->disconnectFromServer();
        m_socket->deleteLater();
        m_socket = NULL;
    }
    m_socket = m_monitorserver->nextPendingConnection();
    connect( m_socket, SIGNAL(readyRead()),
             this, SLOT(readyRead()) );

    loadMonitorSettings();
}

void ClipboardServer::readyRead()
{
    m_socket->blockSignals(true);
    while ( m_socket && m_socket->bytesAvailable() ) {
        ClipboardItem item;
        QByteArray msg;
        if( !readMessage(m_socket, &msg) ) {
            // something is wrong with the connection -> restart monitor
            log( tr("Incorrect message from Clipboard Monitor."), LogError );
            stopMonitoring();
            startMonitoring();
            return;
        }
        QDataStream in(&msg, QIODevice::ReadOnly);
        in >> item;
        if ( m_lastHash == item.dataHash() )
            continue;
        m_lastHash = item.dataHash();

        m_wnd->addToTab( item.data() );
    }
    if (m_socket)
        m_socket->blockSignals(false);
}

void ClipboardServer::changeClipboard(const ClipboardItem *item)
{
    if ( !m_socket || !m_socket->isWritable() )
        return;

    QByteArray msg;
    QDataStream out(&msg, QIODevice::WriteOnly);
    out << *item;
    writeMessage(m_socket, msg);
}

ClipboardServer::CommandStatus ClipboardServer::doCommand(
        Arguments &args, QByteArray *response, ClipboardBrowser *target_browser)
{
    QString cmd;
    args >> cmd;
    if ( args.error() )
        return CommandBadSyntax;

    ClipboardBrowser *c = (target_browser == NULL) ? m_wnd->browser(0) :
                                                     target_browser;
    ClipboardBrowser::Lock lock(c);
    QString mime;
    QMimeData *data;
    int row;

    // show/hide main window
    if (cmd == "show") {
        if ( !args.atEnd() )
            return CommandBadSyntax;
        m_wnd->showWindow();
        response->append(QString::number((qlonglong)m_wnd->winId()));
    }
    else if (cmd == "hide") {
        if ( !args.atEnd() )
            return CommandBadSyntax;
        m_wnd->close();
    }
    else if (cmd == "toggle") {
        if ( !args.atEnd() )
            return CommandBadSyntax;
        m_wnd->toggleVisible();
        if ( m_wnd->isVisible() )
            response->append(QString::number((qlonglong)m_wnd->winId()));
    }

    // exit server
    else if (cmd == "exit") {
        if ( !args.atEnd() )
            return CommandBadSyntax;
        // close client and exit (respond to client first)
        *response = tr("Terminating server.\n").toLocal8Bit();
        QTimer *timer = new QTimer(this);
        timer->start(0);
        connect( timer, SIGNAL(timeout()), SLOT(quit()) );
    }

    // show menu
    else if (cmd == "menu") {
        if ( !args.atEnd() )
            return CommandBadSyntax;
        response->append(QString::number((qlonglong)m_wnd->showMenu()));
    }

    // show action dialog or run action on item
    // action
    // action [[row] ... ["cmd" "[sep]"]]
    else if (cmd == "action") {
        args >> 0 >> row;
        QString text = c->itemText(row);
        while ( !args.finished() ) {
            args >> row;
            if (args.error())
                break;
            text.append( '\n'+c->itemText(row) );
        }

        if ( !args.error() ) {
            m_wnd->openActionDialog(text);
        } else {
            QString cmd, sep;

            args.back();
            args >> cmd >> QString('\n') >> sep;

            if ( !args.finished() )
                return CommandBadSyntax;

            Command command;
            command.cmd = cmd;
            command.output = true;
            command.input = true;
            command.sep = sep;
            command.wait = false;
            command.outputTab = c->getID();
            m_wnd->action(text, command);
        }
    }

    // add new items
    else if (cmd == "add") {
        if ( args.atEnd() )
            return CommandBadSyntax;

        while( !args.atEnd() ) {
            c->add( args.toString(), true );
        }

        c->updateClipboard();
        c->delayedSaveItems(1000);
    }

    // add new items
    else if (cmd == "write") {
        data = new QMimeData;
        do {
            QByteArray bytes;
            args >> mime >> bytes;

            if ( args.error() ) {
                delete data;
                data = NULL;
                return CommandBadSyntax;
            }

            data->setData( mime, bytes );
        } while ( !args.atEnd() );

        c->add(data, true);

        c->updateClipboard();
        c->delayedSaveItems(1000);
    }

    // edit clipboard item
    // edit [row=0] ...
    else if (cmd == "edit") {
        args >> 0 >> row;
        QString text = c->itemText(row);
        bool multiple_edit = !args.finished();
        while ( !args.finished() ) {
            args >> row;
            if (args.error())
               return CommandBadSyntax;
            text.append( '\n'+c->itemText(row) );
        }

        if ( !c->openEditor(text) ) {
            m_wnd->showBrowser(c);
            if ( multiple_edit || row >= c->length() ) {
                c->newItem(text);
                c->edit( c->index(0) );
            } else {
                QModelIndex index = c->index(row);
                c->setCurrent(row);
                c->scrollTo(index, QAbstractItemView::PositionAtTop);
                c->edit(index);
            }
        }
    }

    // set current item
    // select [row=0]
    else if (cmd == "select") {
        args >> 0 >> row;
        if ( !args.finished() )
            return CommandBadSyntax;
        c->moveToClipboard(row);
        c->delayedSaveItems(1000);
    }

    // remove item from clipboard
    // remove [row=0] ...
    else if (cmd == "remove") {
        QList<int> rows;
        rows.reserve( args.length() );

        args >> 0 >> row;
        rows << row;
        while ( !args.finished() ) {
            args >> row;
            if ( args.error() )
                return CommandBadSyntax;
            rows << row;
        }

        qSort( rows.begin(), rows.end(), qGreater<int>() );

        foreach (int row, rows)
            c->model()->removeRow(row);

        if (rows.last() == 0)
            c->updateClipboard();
        c->delayedSaveItems(1000);
    }

    else if (cmd == "length" || cmd == "size" || cmd == "count") {
        if ( args.finished() ) {
            *response = QString("%1\n").arg(c->length()).toLocal8Bit();
        } else {
            return CommandBadSyntax;
        }
    }

    // read [mime="text/plain"|row] ...
    else if (cmd == "read") {
        mime = QString("text/plain");

        if ( args.atEnd() ) {
            const QMimeData *data = clipboardData();
            if (data)
                response->append( data->data(mime) );
        } else {
            do {
                args >> row;
                if ( args.error() ) {
                    args.back();
                    args >> mime;
                    args >> -1 >> row;
                }

                const QMimeData *data = (row >= 0) ?
                            c->itemData(row) : clipboardData();

                if (data) {
                    if (mime == "?")
                        response->append( data->formats().join("\n")+'\n' );
                    else
                        response->append( data->data(mime) );
                }
            } while( !args.atEnd() );
        }
    }

    // clipboard [mime="text/plain"]
    else if (cmd == "clipboard") {
        args >> QString("text/plain") >> mime;
        const QMimeData *data = clipboardData();
        if (data) {
            if (mime == "?")
                response->append( data->formats().join("\n")+'\n' );
            else
                response->append( data->data(mime) );
        }
    }

#ifdef Q_WS_X11
    // selection [mime="text/plain"]
    else if (cmd == "selection") {
        args >> QString("text/plain") >> mime;
        const QMimeData *data = clipboardData(QClipboard::Selection);
        if (data) {
            if (mime == "?")
                response->append( data->formats().join("\n")+'\n' );
            else
                response->append( data->data(mime) );
        }
    }
#endif

    // config [option [value]]
    else if (cmd == "config") {
        ConfigurationManager *cm = ConfigurationManager::instance();

        if ( args.atEnd() ) {
            QStringList options = cm->options();
            options.sort();
            foreach (const QString &option, options) {
                response->append( option + "\n  " +
                                  cm->optionToolTip(option) + '\n' );
            }
        } else {
            QString option;
            args >> option;
            if ( cm->options().contains(option) ) {
                if ( args.atEnd() ) {
                    response->append( cm->value(option).toString()+'\n' );

                } else if ( cm->isVisible() ) {
                    response->append( tr("To modify options from command line "
                                         "you must first close the CopyQ "
                                         "Configuration dialog!\n") );
                    return CommandError;
                } else {
                    QString value;
                    args >> value;
                    if ( !args.atEnd() )
                        return CommandBadSyntax;
                    cm->setValue(option, value);
                    cm->saveSettings();
                }
            } else {
                response->append( tr("Invalid option!\n") );
                return CommandError;
            }
        }
    }

    // tab [tab_name [COMMANDs]]
    else if(cmd == "tab") {
        if ( args.atEnd() ) {
            c = m_wnd->browser(0);
            foreach ( const QString &tabName, m_wnd->tabs() )
                response->append(tabName + '\n');
        } else {
            QString name;
            args >> name;

            if (name.isEmpty()) {
                response->append( tr("Tab name cannot be empty!\n") );
                return CommandError;
            }

            c = m_wnd->createTab(name, true);
            if ( !args.atEnd() )
                return doCommand(args, response, c);
        }
    }

    // removetab! tab_name
    else if(cmd == "removetab!") {
        if ( args.atEnd() )
            return CommandBadSyntax;

        QString name;
        args >> name;

        if ( !args.atEnd() )
            return CommandBadSyntax;

        int i = 0;
        for( c = m_wnd->browser(0); c != NULL && name != c->getID();
             c = m_wnd->browser(++i) );
        if (c == NULL) {
            response->append( tr("Tab with given name doesn't exist!\n") );
            return CommandError;
        }

        m_wnd->removeTab(false, i);
    }

    // renametab tab_name new_tab_name
    else if(cmd == "renametab") {
        if ( args.atEnd() )
            return CommandBadSyntax;

        QString name, new_name;
        args >> name >> new_name;

        if ( args.error() )
            return CommandBadSyntax;

        int i = m_wnd->tabs().indexOf(name);
        if ( i < 0 ) {
            response->append( tr("Tab with given name doesn't exist!\n") );
            return CommandError;
        } else if ( new_name.isEmpty() ) {
            response->append( tr("Tab name cannot be empty!\n") );
            return CommandError;
        } else if ( m_wnd->tabs().indexOf(new_name) >= 0 ) {
            response->append( tr("Tab with given name already exists!\n") );
            return CommandError;
        }

        m_wnd->renameTab(new_name, i);
    }

    // export filename
    else if(cmd == "export") {
        if ( args.atEnd() )
            return CommandBadSyntax;

        QString fileName;
        args >> fileName;

        if ( args.error() || !args.atEnd() )
            return CommandBadSyntax;

        if ( !m_wnd->saveTab( fileName, m_wnd->tabIndex(c) ) ) {
            response->append( tr("Cannot save to file \"%1\"!\n").arg(fileName) );
            return CommandError;
        }
    }

    // import filename
    else if(cmd == "import") {
        if ( args.atEnd() )
            return CommandBadSyntax;

        QString fileName;
        args >> fileName;

        if ( args.error() || !args.atEnd() )
            return CommandBadSyntax;

        if ( !m_wnd->loadTab(fileName) ) {
            response->append( tr("Cannot import file \"%1\"!\n").arg(fileName) );
            return CommandError;
        }
    }

    // unknown command
    else {
        return CommandBadSyntax;
    }

    return CommandSuccess;
}

Arguments *ClipboardServer::createGlobalShortcut(const QString &shortcut)
{
#ifdef NO_GLOBAL_SHORTCUTS
    return NULL;
#else
    if ( shortcut.isEmpty() )
        return NULL;

    QKeySequence keyseq(shortcut, QKeySequence::NativeText);
    QxtGlobalShortcut *s = new QxtGlobalShortcut(keyseq, this);
    connect( s, SIGNAL(activated(QxtGlobalShortcut*)),
             this, SLOT(shortcutActivated(QxtGlobalShortcut*)) );

    return &m_shortcutActions[s];
#endif
}

void ClipboardServer::loadSettings()
{
#ifndef NO_GLOBAL_SHORTCUTS
    ConfigurationManager *cm = ConfigurationManager::instance();

    // set global shortcuts
    QString key;
    Arguments *args;

    foreach (QxtGlobalShortcut *s, m_shortcutActions.keys())
        delete s;
    m_shortcutActions.clear();

    key = cm->value("toggle_shortcut").toString();
    args = createGlobalShortcut(key);
    if (args)
        args->append("toggle");

    key = cm->value("menu_shortcut").toString();
    args = createGlobalShortcut(key);
    if (args)
        args->append("menu");

    key = cm->value("edit_shortcut").toString();
    args = createGlobalShortcut(key);
    if (args)
        args->append("edit");

    key = cm->value("second_shortcut").toString();
    args = createGlobalShortcut(key);
    if (args) {
        args->append("select");
        args->append("1");
    }
#endif

    // reload clipboard monitor configuration
    if ( isMonitoring() )
        loadMonitorSettings();
}

void ClipboardServer::shortcutActivated(QxtGlobalShortcut *shortcut)
{
    Arguments args = m_shortcutActions[shortcut];
    QByteArray response;
    doCommand(args, &response);
}
