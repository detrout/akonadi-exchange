/*
 * This file is part of the Akonadi Exchange Resource.
 * Copyright 2011 Robert Gruber <rgruber@users.sourceforge.net>
 *
 * Akonadi Exchange Resource is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Akonadi Exchange Resource is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Akonadi Exchange Resource.
 * If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef CREATEPROFILE_H
#define CREATEPROFILE_H

#include <QDialog>

#include "ui_createprofiledialog.h"

class CreateProfileDialog : public QDialog, public Ui::CreateProfileDialogBase
{
Q_OBJECT
public:
    explicit CreateProfileDialog(QWidget* parent = 0, Qt::WindowFlags f = 0);
    virtual ~CreateProfileDialog() {}

    QString profileName() const;
    QString username() const;
    QString password() const;
    QString server() const;
    QString domain() const;

private slots:
    void slotValidateData();
};

#endif // CREATEPROFILE_H
