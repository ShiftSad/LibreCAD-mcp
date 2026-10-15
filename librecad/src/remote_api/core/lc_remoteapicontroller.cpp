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

#include "lc_remoteapicontroller.h"

// Minimal, always-disabled controller; see the note in the header.

LC_RemoteApiController::LC_RemoteApiController(QC_ApplicationWindow* appWindow)
    : m_appWindow(appWindow) {
}

LC_RemoteApiController::~LC_RemoteApiController() {
    stop();
}

void LC_RemoteApiController::startIfEnabled() {
}

void LC_RemoteApiController::applySettings() {
}

void LC_RemoteApiController::stop() {
}

QString LC_RemoteApiController::statusText() const {
    return QStringLiteral("Disabled");
}
