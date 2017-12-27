/*
    Copyright (c) 2017, Lukas Holecek <hluk@email.cz>

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

#ifndef DUMMYCLIPBOARD_H
#define DUMMYCLIPBOARD_H

#include "common/clipboardmode.h"
#include "platform/platformclipboard.h"

#include <QClipboard>

class DummyClipboard : public PlatformClipboard
{
    Q_OBJECT
public:
    explicit DummyClipboard(bool connectClipboardSignal = true);

    void loadSettings(const QVariantMap &) override {}

    QVariantMap data(ClipboardMode mode, const QStringList &formats) const override;

    void setData(ClipboardMode mode, const QVariantMap &dataMap) override;

protected:
    virtual void onChanged(int mode);

signals:
    void changed(ClipboardMode mode);

private slots:
    void onChanged(QClipboard::Mode mode);
};

#endif // DUMMYCLIPBOARD_H
