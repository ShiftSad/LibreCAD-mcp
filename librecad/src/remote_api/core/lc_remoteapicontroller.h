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

#include <memory>

#include <QJsonObject>
#include <QString>

#include "lc_remoteapiconfig.h"
#include "lc_remoteapirouter.h"

class LC_RemoteApiContext;
class LC_RemoteApiServer;
class QC_ApplicationWindow;

/**
 * Lives on the GUI thread. The constructor registers the routes: /status and
 * /routes here, then each component's lcRegister*Routes when it is built
 * (LC_REMOTE_API_HAS_ENTITIES, LC_REMOTE_API_HAS_DOCUMENTS). Starting writes
 * the discovery file; stopping removes it when it is still ours.
 * appWindow may be null (tests): no documents, no command-history notices.
 */
class LC_RemoteApiController {
public:
    enum class State { Disabled, Listening, PortInUse, Failed };

    static constexpr auto ApiVersion = "1.0";

    explicit LC_RemoteApiController(QC_ApplicationWindow* appWindow);
    ~LC_RemoteApiController();
    LC_RemoteApiController(const LC_RemoteApiController&) = delete;
    LC_RemoteApiController& operator=(const LC_RemoteApiController&) = delete;

    void startIfEnabled();          // QC_ApplicationWindow::initCompleted()
    void applySettings();           // Preferences accepted: restart only if Enabled, Port or Token changed
    void stop();                    // idempotent; removes the discovery file
    QString statusText() const;     // "Listening on 127.0.0.1:13140", "Disabled", "Port 13140 is in use"
    QC_ApplicationWindow* appWindow() const { return m_appWindow; }

    State state() const { return m_state; }
    bool isListening() const;
    int port() const;                                   // the port listened on; 0 when not listening
    QString discoveryFile() const { return m_discoveryFile; } // written while listening; empty otherwise
    const LC_RemoteApiRouter& router() const { return m_router; }
    LC_RemoteApiContext& context() const { return *m_context; }

    static QString appVersion();    // LC_VERSION, e.g. "2.2.2.6-alpha"
    static int mcpBuild();          // LC_MCP_RELEASE

private:
    void registerMetaRoutes();
    bool start(const LC_RemoteApiConfig& config);
    QJsonObject statusJson() const; // GUI thread

    QC_ApplicationWindow* m_appWindow = nullptr;
    std::unique_ptr<LC_RemoteApiContext> m_context;
    LC_RemoteApiRouter m_router;
    std::unique_ptr<LC_RemoteApiServer> m_server;
    LC_RemoteApiConfig m_running;   // the settings the server runs with
    State m_state = State::Disabled;
    int m_failedPort = 0;
    QString m_failure;
    QString m_discoveryFile;
};

#endif
