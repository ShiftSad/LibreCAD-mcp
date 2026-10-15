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

#include "lc_remoteapiserver.h"

#include <chrono>

#include <QDeadlineTimer>
#include <QHostAddress>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>

#include "lc_remoteapiconfig.h"
#include "lc_remoteapigui.h"
#include "lc_remoteapihttp.h"
#include "lc_remoteapirouter.h"

namespace {
constexpr int PollSliceMs = 100;      // how often the loops look at the stop flag
constexpr int AcceptSliceMs = 200;
constexpr int BindWaitMs = 2000;
constexpr int StopWaitMs = 2000;
constexpr int DrainMs = 1000;         // reading what is left of a refused request
constexpr int StopFlushMs = 300;      // writing the last response while stopping
constexpr qint64 ReadChunk = 64 * 1024;
} // namespace

LC_RemoteApiServer::LC_RemoteApiServer(const LC_RemoteApiRouter& router, LC_RemoteApiContext& context)
    : m_router(router)
    , m_context(context) {
    setObjectName(QStringLiteral("remote-api"));
}

LC_RemoteApiServer::~LC_RemoteApiServer() {
    stop();
}

LC_RemoteApiServer::StartResult LC_RemoteApiServer::start(const quint16 port, const QString& token, QString* error) {
    auto fail = [error](const StartResult result, const QString& why) {
        if (error != nullptr) {
            *error = why;
        }
        return result;
    };
    if (isRunning()) {
        return fail(StartResult::Failed, QStringLiteral("the server is already running"));
    }
    if (token.trimmed().isEmpty()) {
        return fail(StartResult::Failed, QStringLiteral("no API token"));
    }
    m_token = token.trimmed().toUtf8();
    m_requestedPort = port;
    m_stop = false;
    m_listening = false;
    m_port = 0;
    m_bindPromise = std::make_shared<std::promise<BindOutcome>>();
    std::future<BindOutcome> bound = m_bindPromise->get_future();

    QThread::start();
    if (bound.wait_for(std::chrono::milliseconds(BindWaitMs)) != std::future_status::ready) {
        stop();
        return fail(StartResult::Failed, QStringLiteral("binding the port timed out"));
    }
    const BindOutcome outcome = bound.get();
    if (outcome.result != StartResult::Listening) {
        wait();
        return fail(outcome.result, outcome.error);
    }
    return StartResult::Listening;
}

void LC_RemoteApiServer::stop() {
    m_stop = true;
    if (!isRunning()) {
        m_listening = false;
        return;
    }
    // The server thread may be waiting for the GUI thread, which is here.
    LC_RemoteApiGui::abortWaiting();
    if (!wait(StopWaitMs)) {
        qWarning("remote-api: the server thread did not stop within %d ms; waiting for it", StopWaitMs);
        wait();
    }
    m_listening = false;
}

void LC_RemoteApiServer::run() {
    QTcpServer server;
    server.setMaxPendingConnections(16);
    if (!server.listen(QHostAddress::LocalHost, m_requestedPort)) {
        BindOutcome outcome;
        outcome.result = server.serverError() == QAbstractSocket::AddressInUseError ? StartResult::PortInUse
                                                                                     : StartResult::Failed;
        outcome.error = server.errorString();
        m_bindPromise->set_value(outcome);
        return;
    }
    m_port = server.serverPort();
    m_listening = true;
    m_bindPromise->set_value(BindOutcome{StartResult::Listening, {}});

    while (!m_stop.load()) {
        bool timedOut = false;
        if (!server.waitForNewConnection(AcceptSliceMs, &timedOut)) {
            if (!timedOut) {
                msleep(PollSliceMs); // a socket error: do not spin
            }
            continue;
        }
        while (QTcpSocket* socket = server.nextPendingConnection()) {
            serveConnection(*socket);
            delete socket;
            if (m_stop.load()) {
                break;
            }
        }
    }
    m_listening = false;
    server.close();
}

void LC_RemoteApiServer::serveConnection(QTcpSocket& socket) {
    LC_HttpRequestParser parser(m_limits.maxHeaderBytes, m_limits.maxBodyBytes);
    const QDeadlineTimer deadline(m_limits.readTimeoutMs);
    bool timedOut = false;
    while (parser.state() == LC_HttpRequestParser::State::Incomplete) {
        if (m_stop.load()) {
            socket.abort();
            return;
        }
        if (socket.bytesAvailable() > 0) {
            parser.feed(socket.read(ReadChunk));
            continue;
        }
        if (socket.state() != QAbstractSocket::ConnectedState) {
            if (parser.isEmpty()) {
                return; // opened and closed without a request
            }
            parser.finish();
            break;
        }
        if (deadline.hasExpired()) {
            timedOut = true;
            break;
        }
        socket.waitForReadyRead(static_cast<int>(qMin<qint64>(PollSliceMs, qMax<qint64>(1, deadline.remainingTime()))));
    }

    LC_ApiResponse response;
    bool requestComplete = false;
    if (timedOut) {
        response = LC_ApiError(408, QStringLiteral("request_timeout"),
                               QStringLiteral("the request was not received within %1 s").arg(m_limits.readTimeoutMs / 1000.0))
                       .toResponse();
    }
    else if (parser.state() == LC_HttpRequestParser::State::Error) {
        response = parser.error().toResponse();
    }
    else {
        requestComplete = true;
        response = handle(parser.takeRequest());
    }

    if (!writeAll(socket, LC_RemoteApiHttp::serializeResponse(response))) {
        socket.abort();
        return;
    }
    if (requestComplete) {
        socket.disconnectFromHost();
    }
    else {
        drainAndClose(socket);
    }
}

bool LC_RemoteApiServer::writeAll(QTcpSocket& socket, const QByteArray& data) {
    if (socket.state() != QAbstractSocket::ConnectedState || socket.write(data) != data.size()) {
        return false;
    }
    QDeadlineTimer deadline(m_limits.writeTimeoutMs);
    bool stopping = false;
    while (socket.bytesToWrite() > 0) {
        if (m_stop.load() && !stopping) {
            // still deliver this response (often the 503 of the stop itself),
            // but do not hold the stop up for a slow reader
            stopping = true;
            if (deadline.remainingTime() > StopFlushMs || deadline.isForever()) {
                deadline = QDeadlineTimer(StopFlushMs);
            }
        }
        if (deadline.hasExpired() || socket.state() != QAbstractSocket::ConnectedState) {
            return false;
        }
        socket.waitForBytesWritten(PollSliceMs);
    }
    return true;
}

// A refused request may still be arriving. Closing a socket with unread data
// resets the connection on some systems, and the client then loses the
// response; read what comes for a short while first.
void LC_RemoteApiServer::drainAndClose(QTcpSocket& socket) {
    const QDeadlineTimer deadline(DrainMs);
    while (!deadline.hasExpired() && !m_stop.load() && socket.state() == QAbstractSocket::ConnectedState) {
        if (socket.bytesAvailable() > 0) {
            socket.read(ReadChunk);
            continue;
        }
        if (!socket.waitForReadyRead(PollSliceMs)) {
            break; // quiet or closed
        }
    }
    socket.disconnectFromHost();
}

bool LC_RemoteApiServer::hostAllowed(const QByteArray& host) const {
    const QByteArray port = QByteArray::number(m_port.load());
    const QByteArray value = host.trimmed().toLower();
    return value == "127.0.0.1:" + port || value == "localhost:" + port || value == "[::1]:" + port;
}

LC_ApiResponse LC_RemoteApiServer::handle(const LC_ApiRequest& request) const {
    try {
        if (!hostAllowed(request.headers.value(QByteArrayLiteral("host")))) {
            return LC_ApiError(421, QStringLiteral("bad_host"),
                               QStringLiteral("the Host header must be 127.0.0.1:%1 or localhost:%1").arg(m_port.load()))
                .toResponse();
        }
        if (request.headers.contains(QByteArrayLiteral("origin"))) {
            return LC_ApiError(403, QStringLiteral("origin_not_allowed"),
                               QStringLiteral("requests from web pages are not accepted"))
                .toResponse();
        }

        QByteArray given = request.headers.value(QByteArrayLiteral("x-api-token")).trimmed();
        if (given.isEmpty()) {
            const QByteArray authorization = request.headers.value(QByteArrayLiteral("authorization")).trimmed();
            if (authorization.size() > 7 && authorization.left(7).toLower() == "bearer ") {
                given = authorization.mid(7).trimmed();
            }
        }
        if (!LC_RemoteApiConfig::tokenEquals(m_token, given)) {
            LC_ApiResponse response =
                LC_ApiError(401, QStringLiteral("unauthorized"),
                            given.isEmpty() ? QStringLiteral("no API token (send X-Api-Token or Authorization: Bearer)")
                                            : QStringLiteral("wrong API token"),
                            {{QStringLiteral("app"), QStringLiteral("LibreCAD")},
                             {QStringLiteral("hint"),
                              QStringLiteral("the token is in Preferences > Remote API (MCP) and in remote-api.json")}})
                    .toResponse();
            response.extraHeaders.append({QByteArrayLiteral("WWW-Authenticate"), QByteArrayLiteral("Bearer realm=\"LibreCAD\"")});
            return response;
        }

        return m_router.dispatch(request, m_context);
    }
    catch (const LC_ApiError& e) {
        return e.toResponse();
    }
    catch (const std::exception& e) {
        qWarning("remote-api: request failed: %s", e.what());
        return LC_ApiError(500, QStringLiteral("internal"), QString::fromLocal8Bit(e.what())).toResponse();
    }
    catch (...) {
        qWarning("remote-api: request failed with an unknown exception");
        return LC_ApiError(500, QStringLiteral("internal"), QStringLiteral("unknown exception")).toResponse();
    }
}
