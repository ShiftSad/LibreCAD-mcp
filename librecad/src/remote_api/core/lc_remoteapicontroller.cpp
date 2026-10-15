/*******************************************************************************
 *
 This file is part of the LibreCAD project, a 2D CAD program

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

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QSysInfo>
#include <QWidget>

#include "lc_remoteapicontext.h"
#include "lc_remoteapigui.h"
#include "lc_remoteapiroutes.h"
#include "lc_remoteapiserver.h"
#include "qc_applicationwindow.h"
#include "qc_mdiwindow.h"
#include "qg_graphicview.h"
#include "rs_actioninterface.h"
#include "rs_debug.h"

#ifndef LC_MCP_RELEASE
#define LC_MCP_RELEASE 0
#endif

#define LC_REMOTE_API_STR(x) #x
#define LC_REMOTE_API_XSTR(x) LC_REMOTE_API_STR(x)

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("LC_RemoteApiController", text);
}

QString platformName() {
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#else
    return QSysInfo::kernelType();
#endif
}

// Unknown query parameters are refused, as everywhere (doc is always accepted).
void allowNoQuery(const LC_ApiRequest& request) {
    for (auto it = request.query.cbegin(); it != request.query.cend(); ++it) {
        if (it.key() != QLatin1String("doc")) {
            throw LC_ApiError(400, QStringLiteral("unknown_field"), QStringLiteral("unknown query parameter '%1'").arg(it.key()),
                              {{QStringLiteral("field"), it.key()}, {QStringLiteral("allowed"), QJsonArray{QStringLiteral("doc")}}});
        }
    }
}

QString activeActionName(const QG_GraphicView* view) {
    if (view == nullptr) {
        return {};
    }
    RS_ActionInterface* current = view->getCurrentAction();
    if (current == nullptr || current == view->getDefaultAction()) {
        return {};
    }
    const QString name = view->getCurrentActionName();
    return name.isEmpty() ? current->getName() : name;
}

} // namespace

LC_RemoteApiController::LC_RemoteApiController(QC_ApplicationWindow* appWindow)
    : m_appWindow(appWindow)
    , m_context(std::make_unique<LC_RemoteApiContext>(appWindow)) {
    registerMetaRoutes();
#ifdef LC_REMOTE_API_HAS_ENTITIES
    lcRegisterEntityRoutes(m_router);
#endif
#ifdef LC_REMOTE_API_HAS_DOCUMENTS
    lcRegisterDocumentRoutes(m_router);
#endif
    LC_RemoteApiGui::setLateTaskNotifier([context = m_context.get()](const QString& route) {
        context->notify(QStringLiteral("[MCP] %1 finished after the client gave up").arg(route), true);
    });
}

LC_RemoteApiController::~LC_RemoteApiController() {
    stop();
    LC_RemoteApiGui::setLateTaskNotifier({});
}

QString LC_RemoteApiController::appVersion() {
#ifdef LC_VERSION
    return QString::fromLatin1(LC_REMOTE_API_XSTR(LC_VERSION));
#else
    return QCoreApplication::applicationVersion();
#endif
}

int LC_RemoteApiController::mcpBuild() {
    return LC_MCP_RELEASE;
}

QString LC_RemoteApiController::releaseTag() {
#ifdef LC_MCP_TAG
    return QString::fromLatin1(LC_REMOTE_API_XSTR(LC_MCP_TAG));
#else
    // v<version without its label>-mcp.<build>; a git describe value may start with "v"
    QString version = appVersion().section(QLatin1Char('-'), 0, 0);
    if (version.startsWith(QLatin1Char('v'))) {
        version.remove(0, 1);
    }
    return QStringLiteral("v%1-mcp.%2").arg(version).arg(mcpBuild());
#endif
}

QString LC_RemoteApiController::mcpSourceDir() {
#ifdef LC_MCP_SOURCE_DIR
    const QString dir = QString::fromUtf8(LC_MCP_SOURCE_DIR);
    if (QFileInfo::exists(dir + QStringLiteral("/pyproject.toml")) &&
        QFileInfo::exists(dir + QStringLiteral("/src/librecad_mcp/server.py"))) {
        return dir;
    }
#endif
    return {};
}

bool LC_RemoteApiController::isListening() const {
    return m_server != nullptr && m_server->isListening();
}

int LC_RemoteApiController::port() const {
    return isListening() ? m_server->port() : 0;
}

void LC_RemoteApiController::registerMetaRoutes() {
    // /status answers even while the user has a modal dialog open (it reports
    // it), so it posts its own GUI task with ModalPolicy::Allow.
    LC_RouteOptions status;
    status.capability = QStringLiteral("status");
    status.summary = QStringLiteral("App and API version, capabilities, open documents, GUI state");
    status.gui = false;
    status.timeoutMs = 5000;
    m_router.add("GET", QStringLiteral("/api/v1/status"), status, [this, status](LC_ApiCall& call) {
        allowNoQuery(call.request);
        return LC_RemoteApiGui::run([this] { return LC_ApiResponse::ok(statusJson()); }, status.timeoutMs,
                                    QStringLiteral("GET /api/v1/status"), LC_RemoteApiGui::ModalPolicy::Allow);
    });

    LC_RouteOptions routes;
    routes.capability = QStringLiteral("routes");
    routes.summary = QStringLiteral("Every route of this build with its capability and timeout");
    routes.gui = false;
    m_router.add("GET", QStringLiteral("/api/v1/routes"), routes, [this](LC_ApiCall& call) {
        allowNoQuery(call.request);
        return LC_ApiResponse::ok(QJsonObject{{QStringLiteral("api_version"), QLatin1String(ApiVersion)},
                                              {QStringLiteral("routes"), m_router.describe()}});
    });
}

QJsonObject LC_RemoteApiController::statusJson() const {
    const LC_RemoteApiConfig config = m_context->config();

    QStringList capabilities;
    for (const QString& capability : m_router.capabilities()) {
        capabilities << capability;
        if (capability == QLatin1String("export")) {
#ifdef DWGSUPPORT
            capabilities << QStringLiteral("export_dwg");
#endif
            capabilities << QStringLiteral("export_pdf");
        }
    }

    QJsonArray documents;
    QJsonValue activeDocument(QJsonValue::Null);
    QString activeAction;
    QC_MDIWindow* active = m_context->activeDocumentWindow();
    for (QC_MDIWindow* window : m_context->documentWindows()) {
        const QString path = window->getFileName();
        documents.append(QJsonObject{
            {QStringLiteral("id"), static_cast<double>(window->getId())},
            {QStringLiteral("name"), LC_RemoteApiContext::documentName(window)},
            {QStringLiteral("path"), path.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(path)},
            {QStringLiteral("modified"), window->isModified()},
            {QStringLiteral("active"), window == active},
        });
        if (window == active) {
            activeDocument = static_cast<double>(window->getId());
            activeAction = activeActionName(window->getGraphicView());
        }
    }

    const QWidget* modal = qobject_cast<QApplication*>(QCoreApplication::instance()) != nullptr
                               ? QApplication::activeModalWidget()
                               : nullptr;

    return QJsonObject{
        {QStringLiteral("app"), QStringLiteral("LibreCAD")},
        {QStringLiteral("app_version"), appVersion()},
        {QStringLiteral("mcp_build"), mcpBuild()},
        {QStringLiteral("api_version"), QLatin1String(ApiVersion)},
        {QStringLiteral("platform"), platformName()},
        {QStringLiteral("capabilities"), QJsonArray::fromStringList(capabilities)},
#ifdef DWGSUPPORT
        {QStringLiteral("dwg_support"), true},
#else
        {QStringLiteral("dwg_support"), false},
#endif
        {QStringLiteral("commands_allowed"), config.allowCommands},
        {QStringLiteral("discovery_file"),
         m_discoveryFile.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(m_discoveryFile)},
        {QStringLiteral("port"), port()},
        {QStringLiteral("documents"), documents},
        {QStringLiteral("active_document"), activeDocument},
        {QStringLiteral("gui"),
         QJsonObject{
             {QStringLiteral("modal_open"), modal != nullptr},
             {QStringLiteral("modal_window"), modal != nullptr ? QJsonValue(modal->windowTitle()) : QJsonValue(QJsonValue::Null)},
             {QStringLiteral("active_action"), activeAction.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(activeAction)},
         }},
    };
}

void LC_RemoteApiController::startIfEnabled() {
    if (isListening()) {
        return;
    }
    LC_RemoteApiConfig config = LC_RemoteApiConfig::load();
    if (!config.enabled) {
        m_state = State::Disabled;
        m_failure.clear();
        return;
    }
    if (config.token.isEmpty()) {
        config.token = LC_RemoteApiConfig::generateToken();
        if (config.token.isEmpty()) {
            m_state = State::Failed;
            m_failure = tr("no API token could be generated");
            LC_ERR << "remote-api: not started: the system random generator gave no token";
            m_context->notify(tr("[MCP] Remote API not started: %1").arg(m_failure), true);
            return;
        }
        config.save();
    }
    start(config);
}

bool LC_RemoteApiController::start(const LC_RemoteApiConfig& config) {
    auto server = std::make_unique<LC_RemoteApiServer>(m_router, *m_context);
    QString error;
    const LC_RemoteApiServer::StartResult result =
        server->start(static_cast<quint16>(config.port), config.token, &error);
    if (result != LC_RemoteApiServer::StartResult::Listening) {
        m_state = result == LC_RemoteApiServer::StartResult::PortInUse ? State::PortInUse : State::Failed;
        m_failedPort = config.port;
        m_failure = error;
        LC_ERR << "remote-api: not started on 127.0.0.1:" << config.port << ": " << error;
        if (m_state == State::PortInUse) {
            m_context->notify(tr("[MCP] Remote API not started: port %1 is in use (another LibreCAD?)").arg(config.port), true);
        }
        else {
            m_context->notify(tr("[MCP] Remote API not started: %1").arg(error), true);
        }
        return false;
    }

    m_server = std::move(server);
    m_running = config;
    m_state = State::Listening;
    m_failure.clear();

    const int listeningPort = m_server->port();
    const QString path = LC_RemoteApiConfig::discoveryFilePath();
    const QJsonObject discovery{
        {QStringLiteral("url"), QStringLiteral("http://127.0.0.1:%1").arg(listeningPort)},
        {QStringLiteral("port"), listeningPort},
        {QStringLiteral("token"), config.token},
        {QStringLiteral("pid"), static_cast<double>(QCoreApplication::applicationPid())},
        {QStringLiteral("app"), QStringLiteral("LibreCAD")},
        {QStringLiteral("app_version"), appVersion()},
        {QStringLiteral("mcp_build"), mcpBuild()},
        {QStringLiteral("api_version"), QLatin1String(ApiVersion)},
        {QStringLiteral("started_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
    };
    QString discoveryError;
    if (LC_RemoteApiConfig::writeDiscoveryFile(path, discovery, &discoveryError)) {
        m_discoveryFile = path;
    }
    else {
        m_discoveryFile.clear();
        LC_ERR << "remote-api: cannot write the discovery file " << path << ": " << discoveryError;
    }

    LC_LOG(RS_Debug::D_NOTICE) << "remote-api: listening on 127.0.0.1:" << listeningPort;
    m_context->notify(tr("[MCP] Remote API listening on 127.0.0.1:%1").arg(listeningPort));
    return true;
}

void LC_RemoteApiController::applySettings() {
    LC_RemoteApiConfig config = LC_RemoteApiConfig::load();
    if (!config.enabled) {
        stop();
        m_state = State::Disabled;
        m_failure.clear();
        return;
    }
    // Notify, AllowCommands, AllowedRoots and AllowNetworkPaths are read again
    // on every request; only these three need a restart.
    if (isListening() && config.port == m_running.port && config.token == m_running.token) {
        return;
    }
    stop();
    startIfEnabled();
}

void LC_RemoteApiController::stop() {
    if (m_server != nullptr) {
        m_server->stop();
        m_server.reset();
        LC_LOG(RS_Debug::D_NOTICE) << "remote-api: stopped";
    }
    if (!m_discoveryFile.isEmpty()) {
        LC_RemoteApiConfig::removeDiscoveryFile(m_discoveryFile, QCoreApplication::applicationPid());
        m_discoveryFile.clear();
    }
    if (m_state == State::Listening) {
        m_state = State::Disabled;
    }
}

QString LC_RemoteApiController::statusText() const {
    switch (m_state) {
        case State::Listening:
            return tr("Listening on 127.0.0.1:%1").arg(port());
        case State::PortInUse:
            return tr("Port %1 is in use").arg(m_failedPort);
        case State::Failed:
            return tr("Not started: %1").arg(m_failure);
        case State::Disabled:
        default:
            return tr("Disabled");
    }
}
