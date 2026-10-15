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

#ifndef LC_REMOTEAPICONTROLLER_H
#define LC_REMOTEAPICONTROLLER_H

// Owner of the in-app Remote API (design sections 1.1 and 1.3): server thread,
// router, context and settings. QC_ApplicationWindow holds it in m_remoteApi.
//
// For now this is the minimal, always-disabled controller that gives
// QC_ApplicationWindow::m_remoteApi a complete type; the core component
// replaces the implementation and extends the class.

#include <QString>

class QC_ApplicationWindow;

class LC_RemoteApiController {
public:
    explicit LC_RemoteApiController(QC_ApplicationWindow* appWindow);
    ~LC_RemoteApiController();
    LC_RemoteApiController(const LC_RemoteApiController&) = delete;
    LC_RemoteApiController& operator=(const LC_RemoteApiController&) = delete;

    void startIfEnabled();          // QC_ApplicationWindow::initCompleted()
    void applySettings();           // Preferences accepted: restart only if Enabled, Port or Token changed
    void stop();                    // idempotent; removes the discovery file
    QString statusText() const;     // "Listening on 127.0.0.1:13140", "Disabled", "Port 13140 is in use"
    QC_ApplicationWindow* appWindow() const { return m_appWindow; }

private:
    QC_ApplicationWindow* m_appWindow = nullptr;
};

#endif
