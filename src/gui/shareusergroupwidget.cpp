/*
 * Copyright (C) by Roeland Jago Douma <roeland@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "shareusergroupwidget.h"
#include "ui_shareusergroupwidget.h"
#include "ui_sharewidget.h"
#include "account.h"
#include "json.h"
#include "folderman.h"
#include "folder.h"
#include "accountmanager.h"
#include "theme.h"
#include "configfile.h"
#include "capabilities.h"

#include "thumbnailjob.h"
#include "share.h"
#include "sharee.h"

#include "QProgressIndicator.h"
#include <QBuffer>
#include <QFileIconProvider>
#include <QClipboard>
#include <QFileInfo>
#include <QAbstractProxyModel>
#include <QCompleter>
#include <QPropertyAnimation>

namespace OCC {

ShareUserGroupWidget::ShareUserGroupWidget(AccountPtr account, const QString &sharePath, const QString &localPath, bool resharingAllowed, QWidget *parent) :
   QWidget(parent),
    _ui(new Ui::ShareUserGroupWidget),
    _account(account),
    _sharePath(sharePath),
    _localPath(localPath),
    _resharingAllowed(resharingAllowed)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName("SharingDialogUG"); // required as group for saveGeometry call

    _ui->setupUi(this);

    //Is this a file or folder?
    _isFile = QFileInfo(localPath).isFile();

    _completer = new QCompleter(this);
    _ui->shareeLineEdit->setCompleter(_completer);

    _ui->searchPushButton->setEnabled(false);

    _manager = new ShareManager(_account, this);
    connect(_manager, SIGNAL(sharesFetched(QList<QSharedPointer<Share>>)), SLOT(slotSharesFetched(QList<QSharedPointer<Share>>)));
    connect(_manager, SIGNAL(shareCreated(QSharedPointer<Share>)), SLOT(getShares()));
//    connect(_ui->shareeLineEdit, SIGNAL(returnPressed()), SLOT(on_searchPushButton_clicked()));
    connect(_completer, SIGNAL(activated(QModelIndex)), SLOT(slotCompleterActivated(QModelIndex)));

}

ShareUserGroupWidget::~ShareUserGroupWidget()
{
    delete _ui;
}

void ShareUserGroupWidget::on_shareeLineEdit_textChanged(const QString &text)
{
    if (text == "") {
        _ui->searchPushButton->setEnabled(false);
    } else {
        _ui->searchPushButton->setEnabled(true);
    }
}

void ShareUserGroupWidget::on_searchPushButton_clicked()
{
     QVector<QSharedPointer<Sharee>> sharees;

    // Add the current user to _sharees since we can't share with ourself
    QSharedPointer<Sharee> currentUser(new Sharee(_account->credentials()->user(), "", Sharee::Type::User));
    sharees.append(currentUser);

    for(int i = 0; i < _ui->sharesLayout->count(); i++) {
        QWidget *w = _ui->sharesLayout->itemAt(i)->widget();

        if (w != NULL) {
            const QSharedPointer<Sharee> x = static_cast<ShareWidget *>(w)->share()->getShareWith();
            sharees.append(x);
        }
    }

    _sharees.append(currentUser);

    _completerModel = new ShareeModel(_account,
                                      _ui->shareeLineEdit->text(),
                                      _isFile ? QLatin1String("file") : QLatin1String("folder"),
                                      sharees,
                                      _completer);
    connect(_completerModel, SIGNAL(shareesReady()), SLOT(slotUpdateCompletion()));
    _completerModel->fetch();
    _completer->setModel(_completerModel);
}

void ShareUserGroupWidget::slotUpdateCompletion()
{
    _completer->complete();
}

void ShareUserGroupWidget::getShares()
{
    _manager->fetchShares(_sharePath);
}

void ShareUserGroupWidget::slotSharesFetched(const QList<QSharedPointer<Share>> &shares)
{
    /*
     * Delete all current widgets
     */
    QLayoutItem *child;
    while ((child = _ui->sharesLayout->takeAt(0)) != 0) {
        delete child->widget();
    }

    foreach(const auto &share, shares) {
        // We don't handle link shares
        if (share->getShareType() == Share::TypeLink) {
            continue;
        }

        ShareWidget *s = new ShareWidget(share, this);
        _ui->sharesLayout->addWidget(s);
    }
}

void ShareUserGroupWidget::slotCompleterActivated(const QModelIndex & index)
{
    // The index is an index from the QCompletion model which is itelf a proxy
    // model proxying the _completerModel
    auto sharee = qvariant_cast<QSharedPointer<Sharee>>(index.data(Qt::UserRole));
    if (sharee.isNull()) {
        return;
    }

    _manager->createShare(_sharePath, 
                          (Share::ShareType)sharee->type(),
                          sharee->shareWith(),
                          Share::PermissionRead);

    _completer->setModel(NULL);
    _ui->shareeLineEdit->setText(QString());
}

ShareWidget::ShareWidget(QSharedPointer<Share> share,
                                   QWidget *parent) :
  QWidget(parent),
  _ui(new Ui::ShareWidget),
  _share(share),
  _showDetailedPermissions(false)
{
    _ui->setupUi(this);

    _ui->sharedWith->setText(share->getShareWith()->format());

    // Set the permissions checkboxes
    displayPermissions();

    // Hide "detailed permissions" by default
    _ui->permissionDelete->setHidden(true);
    _ui->permissionUpdate->setHidden(true);
    _ui->permissionCreate->setHidden(true);

    connect(_ui->permissionUpdate, SIGNAL(clicked(bool)), SLOT(slotPermissionsChanged()));
    connect(_ui->permissionCreate, SIGNAL(clicked(bool)), SLOT(slotPermissionsChanged()));
    connect(_ui->permissionDelete, SIGNAL(clicked(bool)), SLOT(slotPermissionsChanged()));
    connect(_ui->permissionShare,  SIGNAL(clicked(bool)), SLOT(slotPermissionsChanged()));
    connect(_ui->permissionsEdit,  SIGNAL(clicked(bool)), SLOT(slotEditPermissionsChanged()));

    connect(share.data(), SIGNAL(permissionsSet()), SLOT(slotPermissionsSet()));
    connect(share.data(), SIGNAL(shareDeleted()), SLOT(slotShareDeleted()));
}

void ShareWidget::on_deleteShareButton_clicked()
{
    setEnabled(false);
    _share->deleteShare();
}

void ShareWidget::on_permissionToggleButton_clicked()
{
    _showDetailedPermissions = !_showDetailedPermissions;
    _ui->permissionDelete->setVisible(_showDetailedPermissions);
    _ui->permissionUpdate->setVisible(_showDetailedPermissions);
    _ui->permissionCreate->setVisible(_showDetailedPermissions);

    if (_showDetailedPermissions) {
        _ui->permissionToggleButton->setText("Hide");
    } else {
        _ui->permissionToggleButton->setText("More");
    }
}

ShareWidget::~ShareWidget()
{
    delete _ui;
}

void ShareWidget::slotEditPermissionsChanged()
{
    setEnabled(false);

    Share::Permissions permissions = Share::PermissionRead;

    if (_ui->permissionShare->checkState() == Qt::Checked) {
        permissions |= Share::PermissionUpdate;
    }
    
    if (_ui->permissionsEdit->checkState() == Qt::Checked) {
        permissions |= Share::PermissionCreate;
        permissions |= Share::PermissionUpdate;
        permissions |= Share::PermissionDelete;
    }

    _share->setPermissions(permissions);
}

void ShareWidget::slotPermissionsChanged()
{
    setEnabled(false);
    
    Share::Permissions permissions = Share::PermissionRead;

    if (_ui->permissionUpdate->checkState() == Qt::Checked) {
        permissions |= Share::PermissionUpdate;
    }

    if (_ui->permissionCreate->checkState() == Qt::Checked) {
        permissions |= Share::PermissionCreate;
    }

    if (_ui->permissionDelete->checkState() == Qt::Checked) {
        permissions |= Share::PermissionDelete;
    }

    if (_ui->permissionShare->checkState() == Qt::Checked) {
        permissions |= Share::PermissionShare;
    }

    _share->setPermissions(permissions);
}

void ShareWidget::slotDeleteAnimationFinished()
{
    deleteLater();
}

void ShareWidget::slotShareDeleted()
{
    QPropertyAnimation *animation = new QPropertyAnimation(this, "maximumHeight", this);

    animation->setDuration(500);
    animation->setStartValue(height());
    animation->setEndValue(0);

    connect(animation, SIGNAL(finished()), SLOT(slotDeleteAnimationFinished()));

    animation->start();
}

void ShareWidget::slotPermissionsSet()
{
    displayPermissions();
    setEnabled(true);
}

QSharedPointer<Share> ShareWidget::share() const
{
    return _share;
}

void ShareWidget::displayPermissions()
{
    _ui->permissionCreate->setCheckState(Qt::Unchecked);
    _ui->permissionsEdit->setCheckState(Qt::Unchecked);
    _ui->permissionDelete->setCheckState(Qt::Unchecked);
    _ui->permissionShare->setCheckState(Qt::Unchecked);
    _ui->permissionUpdate->setCheckState(Qt::Unchecked);

    if (_share->getPermissions() & Share::PermissionUpdate) {
        _ui->permissionUpdate->setCheckState(Qt::Checked);
        _ui->permissionsEdit->setCheckState(Qt::Checked);
    }
    if (_share->getPermissions() & Share::PermissionCreate) {
        _ui->permissionCreate->setCheckState(Qt::Checked);
        _ui->permissionsEdit->setCheckState(Qt::Checked);
    }
    if (_share->getPermissions() & Share::PermissionDelete) {
        _ui->permissionDelete->setCheckState(Qt::Checked);
        _ui->permissionsEdit->setCheckState(Qt::Checked);
    }
    if (_share->getPermissions() & Share::PermissionShare) {
        _ui->permissionShare->setCheckState(Qt::Checked);
    }
}

}