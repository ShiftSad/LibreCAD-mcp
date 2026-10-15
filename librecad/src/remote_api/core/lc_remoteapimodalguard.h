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

#ifndef LC_REMOTEAPIMODALGUARD_H
#define LC_REMOTEAPIMODALGUARD_H

#include <QJsonArray>
#include <QObject>

#include "lc_remoteapitypes.h"

class QDialog;

/**
 * Safety net against modal dialogs while a Remote API task runs on the GUI
 * thread (design section 1.7, decision D8).
 *
 * A RAII scope on the GUI thread. While it exists:
 * - every modal QDialog that is shown is recorded as {class, title, text} and
 *   closed on the next turn of its event loop: a QMessageBox by its escape
 *   button, else its first RejectRole, else its first NoRole button, else its
 *   only button, else reject(); any other dialog by reject(). (So a "save
 *   changes?" box answers Cancel, never Discard.) Progress dialogs are left alone,
 *   since rejecting one cancels the work it reports on;
 * - Qt::AA_DontUseNativeDialogs is set, so even a stray QFileDialog is drawn
 *   by Qt and can be closed; the previous value comes back on exit.
 * Scopes nest: only the innermost one acts, and each restores what it found.
 */
class LC_RemoteApiModalGuard : public QObject {
public:
    LC_RemoteApiModalGuard();
    ~LC_RemoteApiModalGuard() override;
    LC_RemoteApiModalGuard(const LC_RemoteApiModalGuard&) = delete;
    LC_RemoteApiModalGuard& operator=(const LC_RemoteApiModalGuard&) = delete;

    // [{class, title, text}] of the dialogs closed so far.
    QJsonArray skippedDialogs() const { return m_skipped; }
    // Adds "skipped_dialogs" to a JSON response when a dialog was closed.
    void attachTo(LC_ApiResponse& response) const;

    // True while a guard exists on the GUI thread.
    static bool isActive();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void dismissLater(QDialog* dialog);

    LC_RemoteApiModalGuard* m_previous = nullptr;
    bool m_previousNoNativeDialogs = false;
    QJsonArray m_skipped;
};

#endif
