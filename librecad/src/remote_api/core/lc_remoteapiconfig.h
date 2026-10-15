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

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

struct LC_RemoteApiConfig {
    static constexpr int DefaultPort = 13140;
    static constexpr int MinPort = 1024;
    static constexpr int MaxPort = 65535;

    bool enabled = false;            // Enabled: off by default
    int port = DefaultPort;          // Port: 1024..65535
    QString token;                   // Token: 32 lowercase hex characters, generated on first enable
    bool notify = true;              // Notify: "[MCP] ..." audit lines in the command history
    bool allowCommands = true;       // AllowCommands: false gives 403 commands_disabled on /command
    QStringList allowedRoots;        // AllowedRoots (stored ';'-separated): empty means any absolute local path
    bool allowNetworkPaths = false;  // AllowNetworkPaths: UNC, \\?\ and device paths

    // ---- settings (GUI thread: RS_Settings is not thread-safe) -------------
    static constexpr const char* SettingsGroup = "RemoteAPI";

    // Reads the group "RemoteAPI". Booleans accept 1/0 and true/false (an INI
    // file written by hand); a port outside 1024..65535 reads as DefaultPort.
    static LC_RemoteApiConfig load();
    // Writes every field; the port only when it is valid.
    void save() const;

    static bool isValidPort(int port);

    // ---- token ------------------------------------------------------------
    // 128 bits from the system CSPRNG as 32 lowercase hex characters. Empty if
    // the generator failed; the server then refuses to start (fail closed).
    static QString generateToken();
    // Constant-time comparison: the length first, then an XOR accumulate over
    // every byte. An empty expected token never matches.
    static bool tokenEquals(const QByteArray& expected, const QByteArray& given);

    // ---- discovery file (design section 1.4) -------------------------------
    // LIBRECAD_SETTINGS_DIR, the folder main() keeps the settings in for a
    // sandboxed run; empty when unset.
    static QString settingsSandboxDir();
    // <LIBRECAD_SETTINGS_DIR>/remote-api.json when that is set, else
    // remote-api.json in QStandardPaths::AppDataLocation
    // (%APPDATA%/LibreCAD/LibreCAD-2 on Windows).
    static QString discoveryFilePath();
    // Writes content atomically (QSaveFile), readable by the owner only.
    static bool writeDiscoveryFile(const QString& path, const QJsonObject& content, QString* error = nullptr);
    // The file's JSON object, or an empty object when missing or unreadable.
    static QJsonObject readDiscoveryFile(const QString& path);
    // Removes the file only when its "pid" is pid, so a second instance never
    // deletes the file of the first. True when the file is gone afterwards.
    static bool removeDiscoveryFile(const QString& path, qint64 pid);
};

#endif
