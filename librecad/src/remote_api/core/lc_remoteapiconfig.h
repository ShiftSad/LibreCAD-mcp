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

#ifndef LC_REMOTEAPICONFIG_H
#define LC_REMOTEAPICONFIG_H

// Settings of the Remote API (RS_Settings group "RemoteAPI", design section 1.4).
//
// The field names are part of the pinned interface: LC_RemoteApiContext::config()
// returns this struct, and route handlers read the policy fields (allowCommands,
// allowedRoots, allowNetworkPaths). The core component adds loading, saving,
// token generation/comparison and the discovery file to this header.

#include <QString>
#include <QStringList>

struct LC_RemoteApiConfig {
    static constexpr int DefaultPort = 13140;

    bool enabled = false;            // Enabled: off by default
    int port = DefaultPort;          // Port: 1024..65535
    QString token;                   // Token: 32 lowercase hex characters, generated on first enable
    bool notify = true;              // Notify: "[MCP] ..." audit lines in the command history
    bool allowCommands = true;       // AllowCommands: false gives 403 commands_disabled on /command
    QStringList allowedRoots;        // AllowedRoots (stored ';'-separated): empty means any absolute local path
    bool allowNetworkPaths = false;  // AllowNetworkPaths: UNC, \\?\ and device paths
};

#endif
