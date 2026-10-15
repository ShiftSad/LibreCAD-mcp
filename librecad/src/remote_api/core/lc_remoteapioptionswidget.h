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

#ifndef LC_REMOTEAPIOPTIONSWIDGET_H
#define LC_REMOTEAPIOPTIONSWIDGET_H

#include <QString>
#include <QWidget>

#include "lc_remoteapiconfig.h"

class LC_RemoteApiController;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

/**
 * The "Remote API (MCP)" tab of Application Preferences (design section 1.5),
 * built in code so that the dialog's .ui file stays untouched.
 *
 * It shows the settings of group "RemoteAPI" and writes them in save(), which
 * QG_DlgOptionsGeneral::ok() calls; QC_ApplicationWindow::slotOptionsGeneral()
 * then applies them through LC_RemoteApiController::applySettings().
 * A regenerated token, like every other change, takes effect on OK.
 */
class LC_RemoteApiOptionsWidget : public QWidget {
    Q_OBJECT
public:
    explicit LC_RemoteApiOptionsWidget(QWidget* parent = nullptr, const LC_RemoteApiController* controller = nullptr);

    void save() const;
    // The settings as the controls show them.
    LC_RemoteApiConfig currentConfig() const;
    // The .mcp.json snippet "Copy MCP config" puts on the clipboard.
    QString mcpConfigSnippet() const;

private slots:
    void onEnableToggled(bool enabled);
    void onShowTokenToggled(bool show);
    void onCopyToken() const;
    void onRegenerateToken();
    void onCopyMcpConfig() const;

private:
    void updateEnabledState();

    QCheckBox* m_cbEnable = nullptr;
    QSpinBox* m_sbPort = nullptr;
    QLineEdit* m_leToken = nullptr;
    QPushButton* m_pbShowToken = nullptr;
    QPushButton* m_pbCopyToken = nullptr;
    QPushButton* m_pbRegenerate = nullptr;
    QPushButton* m_pbCopyConfig = nullptr;
    QLabel* m_lStatus = nullptr;
    QLabel* m_lTokenNote = nullptr;
    QCheckBox* m_cbNotify = nullptr;
    QCheckBox* m_cbAllowCommands = nullptr;
    QLineEdit* m_leAllowedRoots = nullptr;
    QCheckBox* m_cbAllowNetworkPaths = nullptr;
    QString m_token;
};

#endif
