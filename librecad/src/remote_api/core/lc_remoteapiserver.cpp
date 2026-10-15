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

#include "lc_remoteapiserver.h"

#include <chrono>
#include <list>

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
constexpr int BusyWriteMs = 200;      // the 503 to a connection over the limit
constexpr int WatchSliceMs = 50;      // watching the client while its request waits
constexpr qint64 ReadChunk = 64 * 1024;

// Hands each accepted connection over as a bare descriptor, so that its reader
// thread can own the socket (a QTcpSocket belongs to the thread that made it).
class DescriptorServer : public QTcpServer {
public:
    QList<qintptr> takePending() {
        QList<qintptr> pending;
        pending.swap(m_pending);
        return pending;
    }

protected:
    void incomingConnection(const qintptr descriptor) override {
        m_pending.append(descriptor);
    }

private:
    QList<qintptr> m_pending;
};

// The client's own deadline (X-Request-Timeout-Ms), counted from now; an absent
// or malformed value means none.
QDeadlineTimer clientDeadline(const LC_ApiRequest& request) {
    const QByteArray value = request.headers.value(QByteArrayLiteral("x-request-timeout-ms")).trimmed();
    bool ok = false;
    const qint64 ms = value.toLongLong(&ok);
    if (!ok || ms <= 0) {
        return QDeadlineTimer(QDeadlineTimer::Forever);
    }
    return QDeadlineTimer(qMin(ms, LC_RemoteApiServer::MaxClientTimeoutMs));
}

LC_ApiResponse shuttingDown() {
    return LC_ApiError(503, QStringLiteral("shutting_down"), QStringLiteral("the Remote API is stopping")).toResponse();
}

} // namespace

// One request, between its reader thread and the dispatcher.
struct LC_RemoteApiServer::Job {
    LC_ApiRequest request;
    QDeadlineTimer clientDeadline{QDeadlineTimer::Forever};
    std::atomic<bool> clientGone{false};   // the client closed the connection
    std::promise<LC_ApiResponse> promise;
    std::atomic<bool> answered{false};

    void answer(LC_ApiResponse response) {
        bool expected = false;
        if (answered.compare_exchange_strong(expected, true)) {
            promise.set_value(std::move(response));
        }
    }
};

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
    // read here, before any stop() of this run can change it
    m_abortGeneration = LC_RemoteApiGui::abortGeneration();
    {
        const std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
    }
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
    {
        // under the lock, so that the dispatcher cannot miss the wake-up
        const std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stop = true;
    }
    m_queueChanged.notify_all();
    if (!isRunning()) {
        m_listening = false;
        return;
    }
    // The dispatcher may be waiting for the GUI thread, which is here.
    LC_RemoteApiGui::abortWaiting();
    if (!wait(StopWaitMs)) {
        qWarning("remote-api: the server threads did not stop within %d ms; waiting for them", StopWaitMs);
        wait();
    }
    m_listening = false;
}

void LC_RemoteApiServer::run() {
    DescriptorServer server;
    server.setMaxPendingConnections(m_limits.maxConnections + 16);
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

    const std::unique_ptr<QThread> dispatcher(QThread::create([this] { dispatchLoop(); }));
    dispatcher->setObjectName(QStringLiteral("remote-api-dispatch"));
    dispatcher->start();
    m_bindPromise->set_value(BindOutcome{StartResult::Listening, {}});

    std::list<std::unique_ptr<QThread>> readers;
    const auto reap = [&readers] {
        readers.remove_if([](const std::unique_ptr<QThread>& reader) {
            return reader->isFinished();
        });
    };
    while (!m_stop.load()) {
        bool timedOut = false;
        if (!server.waitForNewConnection(AcceptSliceMs, &timedOut) && !timedOut) {
            msleep(PollSliceMs); // a socket error: do not spin
        }
        reap();
        for (const qintptr descriptor : server.takePending()) {
            if (m_stop.load() || static_cast<int>(readers.size()) >= m_limits.maxConnections) {
                refuseBusy(descriptor);
                continue;
            }
            std::unique_ptr<QThread> reader(QThread::create([this, descriptor] { serveConnection(descriptor); }));
            reader->setObjectName(QStringLiteral("remote-api-connection"));
            reader->start();
            readers.push_back(std::move(reader));
        }
    }
    m_listening = false;
    server.close();
    for (const qintptr descriptor : server.takePending()) {
        refuseBusy(descriptor);
    }
    // The dispatcher answers what is still queued (503) before it ends; the
    // readers write those answers and end too.
    m_queueChanged.notify_all();
    dispatcher->wait();
    for (const auto& reader : readers) {
        reader->wait();
    }
}

void LC_RemoteApiServer::refuseBusy(const qintptr descriptor) const {
    QTcpSocket socket;
    if (!socket.setSocketDescriptor(descriptor)) {
        return;
    }
    const LC_ApiResponse response =
        m_stop.load() ? shuttingDown()
                      : LC_ApiError(503, QStringLiteral("server_busy"),
                                    QStringLiteral("too many connections to the Remote API at once"),
                                    {{QStringLiteral("hint"), QStringLiteral("retry in a moment")}})
                            .toResponse();
    socket.write(LC_RemoteApiHttp::serializeResponse(response));
    socket.waitForBytesWritten(BusyWriteMs);
    socket.abort();
}

bool LC_RemoteApiServer::enqueue(const std::shared_ptr<Job>& job) {
    {
        const std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_stop.load()) {
            return false;
        }
        m_queue.push_back(job);
    }
    m_queueChanged.notify_all();
    return true;
}

void LC_RemoteApiServer::dispatchLoop() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueChanged.wait(lock, [this] { return m_stop.load() || !m_queue.empty(); });
            if (m_queue.empty()) {
                return; // stopping, and nothing is left to answer
            }
            job = m_queue.front();
            m_queue.pop_front();
        }
        if (job->clientGone.load()) {
            // the client gave up while the request waited: it never runs
            job->answer(LC_ApiResponse());
            continue;
        }
        if (m_stop.load()) {
            job->answer(shuttingDown());
            continue;
        }
        if (job->clientDeadline.hasExpired()) {
            job->answer(LC_ApiError(504, QStringLiteral("ui_timeout"),
                                    QStringLiteral("the request waited past the client's deadline"),
                                    {{QStringLiteral("started"), false},
                                     {QStringLiteral("hint"), QStringLiteral("LibreCAD is busy; retry in a few seconds")}})
                            .toResponse());
            continue;
        }
        const LC_RemoteApiGui::ClientScope scope(
            m_abortGeneration, [job] { return job->clientGone.load(); }, job->clientDeadline);
        job->answer(dispatchSafely(job->request));
    }
}

void LC_RemoteApiServer::serveConnection(const qintptr descriptor) {
    QTcpSocket socket;
    if (!socket.setSocketDescriptor(descriptor)) {
        return;
    }
    LC_HttpRequestParser parser(m_limits.maxHeaderBytes, m_limits.maxBodyBytes);
    const int headerTimeoutMs = qMin(m_limits.headerTimeoutMs, m_limits.readTimeoutMs);
    const QDeadlineTimer headerDeadline(headerTimeoutMs);
    const QDeadlineTimer readDeadline(m_limits.readTimeoutMs);
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
        const QDeadlineTimer& deadline = parser.headersComplete() ? readDeadline : headerDeadline;
        if (deadline.hasExpired()) {
            timedOut = true;
            break;
        }
        socket.waitForReadyRead(static_cast<int>(qMin<qint64>(PollSliceMs, qMax<qint64>(1, deadline.remainingTime()))));
    }

    if (timedOut || parser.state() == LC_HttpRequestParser::State::Error) {
        const LC_ApiResponse refused =
            timedOut ? LC_ApiError(408, QStringLiteral("request_timeout"),
                                   parser.headersComplete()
                                       ? QStringLiteral("the request was not received within %1 s").arg(m_limits.readTimeoutMs / 1000.0)
                                       : QStringLiteral("the request headers were not received within %1 s").arg(headerTimeoutMs / 1000.0))
                           .toResponse()
                     : parser.error().toResponse();
        if (writeAll(socket, LC_RemoteApiHttp::serializeResponse(refused))) {
            drainAndClose(socket);
        }
        else {
            socket.abort();
        }
        return;
    }

    LC_ApiResponse response;
    const LC_ApiRequest request = parser.takeRequest();
    if (const std::optional<LC_ApiResponse> refused = refusal(request)) {
        response = *refused;
    }
    else if (socket.state() != QAbstractSocket::ConnectedState) {
        return; // the client is gone already: nothing runs for it
    }
    else {
        auto job = std::make_shared<Job>();
        job->request = request;
        job->clientDeadline = clientDeadline(request);
        std::future<LC_ApiResponse> answer = job->promise.get_future();
        if (!enqueue(job)) {
            response = shuttingDown();
        }
        else {
            // Watch the connection while the request waits. A client that closes
            // it gives the request up: it then never runs, or, when it runs
            // already, nobody waits for LibreCAD to finish it.
            QDeadlineTimer stopping(QDeadlineTimer::Forever);
            while (answer.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                if (socket.bytesAvailable() > 0) {
                    socket.read(ReadChunk); // nothing more is expected; ignore it
                }
                if (socket.state() != QAbstractSocket::ConnectedState) {
                    job->clientGone = true;
                    return;
                }
                if (m_stop.load()) {
                    // the dispatcher answers 503 at once; do not wait for ever
                    if (stopping.isForever()) {
                        stopping = QDeadlineTimer(StopWaitMs);
                    }
                    else if (stopping.hasExpired()) {
                        socket.abort();
                        return;
                    }
                }
                socket.waitForReadyRead(WatchSliceMs);
            }
            response = answer.get();
        }
    }

    if (!writeAll(socket, LC_RemoteApiHttp::serializeResponse(response))) {
        socket.abort();
        return;
    }
    socket.disconnectFromHost();
}

bool LC_RemoteApiServer::writeAll(QTcpSocket& socket, const QByteArray& data) const {
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
void LC_RemoteApiServer::drainAndClose(QTcpSocket& socket) const {
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

std::optional<LC_ApiResponse> LC_RemoteApiServer::refusal(const LC_ApiRequest& request) const {
    if (!hostAllowed(request.headers.value(QByteArrayLiteral("host")))) {
        return LC_ApiError(421, QStringLiteral("bad_host"),
                           QStringLiteral("the Host header must be 127.0.0.1:%1 or localhost:%1").arg(m_port.load()))
            .toResponse();
    }
    if (request.headers.contains(QByteArrayLiteral("origin"))) {
        return LC_ApiError(403, QStringLiteral("origin_not_allowed"), QStringLiteral("requests from web pages are not accepted"))
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
    return std::nullopt;
}

LC_ApiResponse LC_RemoteApiServer::dispatchSafely(const LC_ApiRequest& request) const {
    try {
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

LC_ApiResponse LC_RemoteApiServer::handle(const LC_ApiRequest& request) const {
    if (const std::optional<LC_ApiResponse> refused = refusal(request)) {
        return *refused;
    }
    return dispatchSafely(request);
}
