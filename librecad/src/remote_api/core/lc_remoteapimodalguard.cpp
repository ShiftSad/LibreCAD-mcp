/*******************************************************************************
 *
 This file is part of the LibreCAD project, a 2D CAD program

 Copyright (C) 2026 LibreCAD.org
 Copyright (C) 2026 LibreCAD-mcp contributors

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 ******************************************************************************/

#include "lc_remoteapimodalguard.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QDialog>
#include <QEvent>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>

namespace {
// The innermost guard; GUI thread only.
LC_RemoteApiModalGuard* s_current = nullptr;

void closeDialog(QDialog* dialog) {
    if (dialog == nullptr || !dialog->isVisible()) {
        return;
    }
    if (auto* box = qobject_cast<QMessageBox*>(dialog)) {
        // the escape button, as QMessageBox itself detects it: one set
        // explicitly, else a Reject or No button, else the only button
        const QList<QAbstractButton*> buttons = box->buttons();
        QAbstractButton* choice = box->escapeButton();
        for (const QMessageBox::ButtonRole role : {QMessageBox::RejectRole, QMessageBox::NoRole}) {
            for (QAbstractButton* button : buttons) {
                if (choice == nullptr && box->buttonRole(button) == role) {
                    choice = button;
                }
            }
        }
        if (choice == nullptr && buttons.size() == 1) {
            choice = buttons.first();
        }
        if (choice != nullptr) {
            choice->click();
            return;
        }
    }
    dialog->reject();
}
} // namespace

LC_RemoteApiModalGuard::LC_RemoteApiModalGuard()
    : m_previous(s_current)
    , m_previousNoNativeDialogs(QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs)) {
    s_current = this;
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    if (QCoreApplication* app = QCoreApplication::instance()) {
        app->installEventFilter(this);
    }
}

LC_RemoteApiModalGuard::~LC_RemoteApiModalGuard() {
    if (QCoreApplication* app = QCoreApplication::instance()) {
        app->removeEventFilter(this);
    }
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, m_previousNoNativeDialogs);
    s_current = m_previous;
}

bool LC_RemoteApiModalGuard::isActive() {
    return s_current != nullptr;
}

void LC_RemoteApiModalGuard::attachTo(LC_ApiResponse& response) const {
    if (m_skipped.isEmpty()) {
        return;
    }
    if (response.raw.isEmpty()) {
        QJsonArray all = response.json.value(QStringLiteral("skipped_dialogs")).toArray();
        for (const QJsonValue& entry : m_skipped) {
            all.append(entry);
        }
        response.json.insert(QStringLiteral("skipped_dialogs"), all);
    }
    else {
        response.extraHeaders.append({QByteArrayLiteral("X-LibreCAD-Skipped-Dialogs"),
                                      QByteArray::number(m_skipped.size())});
    }
}

bool LC_RemoteApiModalGuard::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Show && s_current == this && watched->isWidgetType()) {
        auto* dialog = qobject_cast<QDialog*>(watched);
        if (dialog != nullptr && dialog->isWindow() && qobject_cast<QProgressDialog*>(dialog) == nullptr &&
            (dialog->isModal() || dialog->windowModality() != Qt::NonModal)) {
            dismissLater(dialog);
        }
    }
    return QObject::eventFilter(watched, event);
}

void LC_RemoteApiModalGuard::dismissLater(QDialog* dialog) {
    QString text;
    if (const auto* box = qobject_cast<QMessageBox*>(dialog)) {
        text = box->text();
        if (!box->informativeText().isEmpty()) {
            text += (text.isEmpty() ? QString() : QStringLiteral("\n")) + box->informativeText();
        }
    }
    m_skipped.append(QJsonObject{
        {QStringLiteral("class"), QString::fromLatin1(dialog->metaObject()->className())},
        {QStringLiteral("title"), dialog->windowTitle()},
        {QStringLiteral("text"), text},
    });
    // Queued: the dialog's own event loop (exec) runs it once it has started.
    QMetaObject::invokeMethod(dialog, [target = QPointer<QDialog>(dialog)] { closeDialog(target.data()); },
                              Qt::QueuedConnection);
}
