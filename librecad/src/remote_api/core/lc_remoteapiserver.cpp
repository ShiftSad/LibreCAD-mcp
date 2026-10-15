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

// The connections are served through the native socket API. QTcpSocket takes
// the end of a client's sending (a half-close) for the end of the connection
// and closes the socket, so a client that half-closes after its request would
// lose its answer.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <climits>
#include <list>

#include <QHostAddress>
#include <QJsonObject>
#include <QTcpServer>

#include "lc_remoteapiconfig.h"
#include "lc_remoteapigui.h"
#include "lc_remoteapihttp.h"
#include "lc_remoteapirouter.h"

namespace {
constexpr int PollSliceMs = 50;       // how often the readers look at the stop flag
constexpr int AcceptSliceMs = 200;
constexpr int DrainSliceMs = 20;      // the accept loop's slice while refused connections drain
constexpr int BindWaitMs = 2000;
constexpr int StopWaitMs = 2000;
constexpr int DrainMs = 1000;         // reading what is left of a refused request
constexpr std::size_t MaxDraining = 64;
constexpr qint64 DrainReadBudget = 1024 * 1024; // per connection and pass
constexpr int StopFlushMs = 300;      // writing the last response while stopping
constexpr int WatchSliceMs = 20;      // looking at the client while its request waits
constexpr qint64 ReadChunk = 64 * 1024;

// Sent once to an HTTP/1.1 client that ends its sending while its request
// waits: a client that closed its socket answers it with a reset (it is gone),
// one that only half-closed reads it before the final answer (RFC 9110 15.2).
const char InterimContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";

// ---- the native socket of a connection --------------------------------------
namespace net {
#ifdef _WIN32
using Native = SOCKET;
bool wouldBlock() {
    return WSAGetLastError() == WSAEWOULDBLOCK;
}
#else
using Native = int;
bool wouldBlock() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}
#endif

Native native(const qintptr descriptor) {
    return static_cast<Native>(descriptor);
}

// Non-blocking, and no SIGPIPE when the client is gone.
void prepare(const qintptr descriptor) {
#ifdef _WIN32
    // An accepted socket inherits the event selection Qt set on the listening
    // socket; drop it, so that the socket can be used on its own.
    WSAEventSelect(native(descriptor), nullptr, 0);
    u_long nonBlocking = 1;
    ioctlsocket(native(descriptor), FIONBIO, &nonBlocking);
#else
    const int flags = fcntl(native(descriptor), F_GETFL, 0);
    if (flags >= 0) {
        fcntl(native(descriptor), F_SETFL, flags | O_NONBLOCK);
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(native(descriptor), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#endif
}

// Waits up to ms until the socket can be read (data, the end of the client's
// sending, or a failure) or, with forWriting, written.
void waitFor(const qintptr descriptor, const bool forWriting, const int ms) {
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(native(descriptor), &set);
    timeval timeout{ms / 1000, (ms % 1000) * 1000};
    select(0, forWriting ? nullptr : &set, forWriting ? &set : nullptr, nullptr, &timeout);
#else
    pollfd watched{native(descriptor), static_cast<short>(forWriting ? POLLOUT : POLLIN), 0};
    while (poll(&watched, 1, ms) < 0 && errno == EINTR) {
    }
#endif
}

// What there is to read, without waiting: > 0 bytes read; 0 the client ended
// its sending; -1 nothing yet; -2 the connection failed.
qint64 read(const qintptr descriptor, char* buffer, const qint64 max) {
    const int size = static_cast<int>(qMin<qint64>(max, INT_MAX));
#ifdef _WIN32
    const int n = ::recv(native(descriptor), buffer, size, 0);
    if (n >= 0) {
        return n;
    }
    return wouldBlock() ? -1 : -2;
#else
    for (;;) {
        const ssize_t n = ::recv(native(descriptor), buffer, static_cast<size_t>(size), 0);
        if (n >= 0) {
            return n;
        }
        if (errno != EINTR) {
            return wouldBlock() ? -1 : -2;
        }
    }
#endif
}

// Sends what the socket takes now: >= 0 bytes sent; -1 none now; -2 the connection failed.
qint64 send(const qintptr descriptor, const char* data, const qint64 size) {
    const int chunk = static_cast<int>(qMin<qint64>(size, INT_MAX));
#ifdef _WIN32
    const int n = ::send(native(descriptor), data, chunk, 0);
    if (n >= 0) {
        return n;
    }
    return wouldBlock() ? -1 : -2;
#else
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    for (;;) {
        const ssize_t n = ::send(native(descriptor), data, static_cast<size_t>(chunk), flags);
        if (n >= 0) {
            return n;
        }
        if (errno != EINTR) {
            return wouldBlock() ? -1 : -2;
        }
    }
#endif
}

void shutdownSending(const qintptr descriptor) {
#ifdef _WIN32
    ::shutdown(native(descriptor), SD_SEND);
#else
    ::shutdown(native(descriptor), SHUT_WR);
#endif
}

void close(const qintptr descriptor) {
#ifdef _WIN32
    ::closesocket(native(descriptor));
#else
    ::close(native(descriptor));
#endif
}

// Closes with a reset: the client sees the connection fail at once.
void abort(const qintptr descriptor) {
    struct linger option{};
    option.l_onoff = 1;
    option.l_linger = 0;
    setsockopt(native(descriptor), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&option), sizeof(option));
    close(descriptor);
}
} // namespace net

// A reader's connection: reset when it goes out of scope, unless it was closed
// or handed on.
class Connection {
public:
    explicit Connection(const qintptr descriptor)
        : m_descriptor(descriptor) {
        net::prepare(descriptor);
    }
    ~Connection() {
        if (m_open) {
            net::abort(m_descriptor);
        }
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    qintptr release() {
        m_open = false;
        return m_descriptor;
    }
    void close() {
        if (m_open) {
            m_open = false;
            net::close(m_descriptor);
        }
    }

private:
    qintptr m_descriptor;
    bool m_open = true;
};

// Hands each accepted connection over as a bare descriptor, so that its reader
// thread can serve it on its own.
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

LC_ApiResponse serverBusy() {
    return LC_ApiError(503, QStringLiteral("server_busy"), QStringLiteral("too many connections to the Remote API at once"),
                       {{QStringLiteral("hint"), QStringLiteral("retry in a moment")}})
        .toResponse();
}

LC_ApiResponse waitedPastDeadline() {
    return LC_ApiError(504, QStringLiteral("ui_timeout"), QStringLiteral("the request waited past the client's deadline"),
                       {{QStringLiteral("started"), false},
                        {QStringLiteral("hint"), QStringLiteral("LibreCAD is busy; retry in a few seconds")}})
        .toResponse();
}

} // namespace

// One request, between its reader thread and the dispatcher.
struct LC_RemoteApiServer::Job {
    enum State : int { Queued, Taken, Dropped };

    LC_ApiRequest request;
    QDeadlineTimer clientDeadline{QDeadlineTimer::Forever};
    std::atomic<bool> clientGone{false};   // the client's connection failed
    std::atomic<int> state{Queued};
    std::promise<LC_ApiResponse> promise;
    std::atomic<bool> answered{false};

    // The dispatcher takes it; false when its reader dropped it first.
    bool take() {
        int expected = Queued;
        return state.compare_exchange_strong(expected, Taken);
    }
    // Its reader drops it (the client is gone, or past its deadline); false
    // when the dispatcher took it first: then it runs.
    bool drop() {
        int expected = Queued;
        return state.compare_exchange_strong(expected, Dropped);
    }
    void answer(LC_ApiResponse response) {
        bool expected = false;
        if (answered.compare_exchange_strong(expected, true)) {
            promise.set_value(std::move(response));
        }
    }
};

// What a reader thread is doing, as the accepting thread sees it.
struct LC_RemoteApiServer::ReaderSlot {
    enum Phase : int {
        Headers,   // its request headers have not all arrived
        Request,   // past its headers and the token: reading, queued, running or answering
        Evicted,   // closed by the accepting thread to make room
        Finished
    };
    std::atomic<int> phase{Headers};
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

    struct Reader {
        std::unique_ptr<QThread> thread;
        std::shared_ptr<ReaderSlot> slot;
    };
    std::list<Reader> readers;
    const int maxConnections = qMax(1, m_limits.maxConnections);
    const auto reap = [&readers] {
        readers.remove_if([](const Reader& reader) {
            return reader.thread->isFinished();
        });
    };
    // Whether a new connection may have a reader. When every reader is busy,
    // the one that has waited longest for its request headers gives way (idle
    // connections cannot lock the real client out); when every one is past its
    // headers, nobody does. Evicted readers end within a poll slice; until then
    // they count against twice the limit.
    const auto makeRoom = [&readers, maxConnections] {
        if (static_cast<int>(readers.size()) >= 2 * maxConnections) {
            return false;
        }
        int serving = 0;
        for (const Reader& reader : readers) {
            const int phase = reader.slot->phase.load();
            serving += phase == ReaderSlot::Headers || phase == ReaderSlot::Request ? 1 : 0;
        }
        if (serving < maxConnections) {
            return true;
        }
        for (Reader& reader : readers) {   // oldest first
            int expected = ReaderSlot::Headers;
            if (reader.slot->phase.compare_exchange_strong(expected, ReaderSlot::Evicted)) {
                return true;
            }
        }
        return false;
    };
    while (!m_stop.load()) {
        bool draining = false;
        {
            const std::lock_guard<std::mutex> lock(m_drainMutex);
            draining = !m_draining.empty();
        }
        bool timedOut = false;
        if (!server.waitForNewConnection(draining ? DrainSliceMs : AcceptSliceMs, &timedOut) && !timedOut) {
            msleep(PollSliceMs); // a socket error: do not spin
        }
        reap();
        for (const qintptr descriptor : server.takePending()) {
            if (m_stop.load()) {
                refuseConnection(descriptor, shuttingDown());
                continue;
            }
            if (!makeRoom()) {
                refuseConnection(descriptor, serverBusy());
                continue;
            }
            auto slot = std::make_shared<ReaderSlot>();
            std::unique_ptr<QThread> thread(QThread::create([this, descriptor, slot] { serveConnection(descriptor, slot); }));
            thread->setObjectName(QStringLiteral("remote-api-connection"));
            thread->start();
            readers.push_back({std::move(thread), std::move(slot)});
        }
        serviceDraining(false);
    }
    m_listening = false;
    server.close();
    for (const qintptr descriptor : server.takePending()) {
        refuseConnection(descriptor, shuttingDown());
    }
    // The dispatcher answers what is still queued (503) before it ends; the
    // readers write those answers and end too.
    m_queueChanged.notify_all();
    dispatcher->wait();
    for (const Reader& reader : readers) {
        reader.thread->wait();
    }
    // the last refusals: read briefly what their clients still send, then close
    const QDeadlineTimer flush(StopFlushMs);
    for (;;) {
        serviceDraining(false);
        {
            const std::lock_guard<std::mutex> lock(m_drainMutex);
            if (m_draining.empty()) {
                break;
            }
        }
        if (flush.hasExpired()) {
            break;
        }
        msleep(DrainSliceMs);
    }
    serviceDraining(true);
}

void LC_RemoteApiServer::refuseConnection(const qintptr descriptor, const LC_ApiResponse& response) {
    net::prepare(descriptor);
    // one try: the answer is small and the new socket's buffer is empty; the
    // accepting thread never waits for a client
    const QByteArray data = LC_RemoteApiHttp::serializeResponse(response);
    net::send(descriptor, data.constData(), data.size());
    net::shutdownSending(descriptor);
    drainLater(descriptor);
}

void LC_RemoteApiServer::refuseAndDrain(const qintptr descriptor, const LC_ApiResponse& response) {
    if (!writeAll(descriptor, LC_RemoteApiHttp::serializeResponse(response))) {
        net::abort(descriptor);
        return;
    }
    net::shutdownSending(descriptor);
    drainLater(descriptor);
}

void LC_RemoteApiServer::drainLater(const qintptr descriptor) {
    std::vector<qintptr> overflow;
    {
        const std::lock_guard<std::mutex> lock(m_drainMutex);
        m_draining.push_back({descriptor, QDeadlineTimer(DrainMs)});
        while (m_draining.size() > MaxDraining) {
            overflow.push_back(m_draining.front().descriptor);
            m_draining.erase(m_draining.begin());
        }
    }
    for (const qintptr closing : overflow) {
        net::close(closing);
    }
}

void LC_RemoteApiServer::serviceDraining(const bool closeAll) {
    std::vector<Draining> current;
    {
        const std::lock_guard<std::mutex> lock(m_drainMutex);
        current.swap(m_draining);
    }
    if (current.empty()) {
        return;
    }
    std::vector<Draining> keep;
    QByteArray buffer(ReadChunk, Qt::Uninitialized);
    for (const Draining& draining : current) {
        bool done = closeAll || draining.until.hasExpired();
        for (qint64 budget = DrainReadBudget; budget > 0;) {
            const qint64 n = net::read(draining.descriptor, buffer.data(), buffer.size());
            if (n > 0) {
                budget -= n;
                continue;
            }
            if (n != -1) {
                done = true; // the client ended its sending, or the connection failed
            }
            break;
        }
        if (done) {
            net::close(draining.descriptor);
        }
        else {
            keep.push_back(draining);
        }
    }
    if (!keep.empty()) {
        const std::lock_guard<std::mutex> lock(m_drainMutex);
        m_draining.insert(m_draining.begin(), keep.cbegin(), keep.cend());
    }
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
        if (!job->take()) {
            continue; // its reader dropped it: the client is gone, or was answered at its deadline
        }
        if (job->clientGone.load()) {
            job->answer(LC_ApiResponse()); // nobody reads it
            continue;
        }
        if (m_stop.load()) {
            job->answer(shuttingDown());
            continue;
        }
        if (job->clientDeadline.hasExpired()) {
            job->answer(waitedPastDeadline());
            continue;
        }
        const LC_RemoteApiGui::ClientScope scope(
            m_abortGeneration, [job] { return job->clientGone.load(); }, job->clientDeadline);
        job->answer(dispatchSafely(job->request));
    }
}

void LC_RemoteApiServer::serveConnection(const qintptr descriptor, const std::shared_ptr<ReaderSlot>& slot) {
    struct Finish {
        ReaderSlot& slot;
        ~Finish() { slot.phase = ReaderSlot::Finished; }
    } finish{*slot};
    Connection connection(descriptor);
    LC_HttpRequestParser parser(m_limits.maxHeaderBytes, m_limits.maxBodyBytes);
    const int headerTimeoutMs = qMin(m_limits.headerTimeoutMs, m_limits.readTimeoutMs);
    const QDeadlineTimer headerDeadline(headerTimeoutMs);
    const QDeadlineTimer readDeadline(m_limits.readTimeoutMs);
    QByteArray buffer(ReadChunk, Qt::Uninitialized);
    bool timedOut = false;
    bool evicted = false;
    bool checked = false; // Host, Origin and the token
    while (parser.state() == LC_HttpRequestParser::State::Incomplete) {
        if (m_stop.load()) {
            return; // reset
        }
        if (slot->phase.load() == ReaderSlot::Evicted) {
            evicted = true;
            break;
        }
        const qint64 n = net::read(descriptor, buffer.data(), buffer.size());
        if (n > 0) {
            parser.feed(QByteArrayView(buffer.constData(), n));
            if (!checked && parser.headersComplete() && parser.state() != LC_HttpRequestParser::State::Error) {
                // before the body: a client without the token never gets to send it
                checked = true;
                if (const std::optional<LC_ApiResponse> refused = refusal(parser.request())) {
                    refuseAndDrain(connection.release(), *refused);
                    return;
                }
                int expected = ReaderSlot::Headers;
                if (!slot->phase.compare_exchange_strong(expected, ReaderSlot::Request)) {
                    evicted = true;
                    break;
                }
            }
            continue;
        }
        if (n == 0) { // the client ended its sending
            if (parser.isEmpty()) {
                connection.close(); // opened and closed without a request
                return;
            }
            parser.finish();
            break;
        }
        if (n == -2) {
            return; // the connection failed
        }
        const QDeadlineTimer& deadline = parser.headersComplete() ? readDeadline : headerDeadline;
        if (deadline.hasExpired()) {
            timedOut = true;
            break;
        }
        net::waitFor(descriptor, false, static_cast<int>(qMin<qint64>(PollSliceMs, qMax<qint64>(1, deadline.remainingTime()))));
    }

    if (evicted) {
        refuseAndDrain(connection.release(),
                       LC_ApiError(408, QStringLiteral("request_timeout"),
                                   QStringLiteral("the request headers had not arrived when the Remote API needed "
                                                  "this connection for another client"))
                           .toResponse());
        return;
    }
    if (timedOut || parser.state() == LC_HttpRequestParser::State::Error) {
        const LC_ApiResponse refused =
            timedOut ? LC_ApiError(408, QStringLiteral("request_timeout"),
                                   parser.headersComplete()
                                       ? QStringLiteral("the request was not received within %1 s").arg(m_limits.readTimeoutMs / 1000.0)
                                       : QStringLiteral("the request headers were not received within %1 s").arg(headerTimeoutMs / 1000.0))
                           .toResponse()
                     : parser.error().toResponse();
        refuseAndDrain(connection.release(), refused);
        return;
    }

    const bool http10 = parser.isHttp10();
    auto job = std::make_shared<Job>();
    job->request = parser.takeRequest();
    job->clientDeadline = clientDeadline(job->request);
    std::future<LC_ApiResponse> answer = job->promise.get_future();
    LC_ApiResponse response;
    if (!enqueue(job)) {
        response = shuttingDown();
    }
    else {
        // Watch the client while the request waits or runs (see the class comment).
        bool ended = false; // the client ended its sending
        QDeadlineTimer stopping(QDeadlineTimer::Forever);
        for (;;) {
            if (answer.wait_for(std::chrono::milliseconds(WatchSliceMs)) == std::future_status::ready) {
                response = answer.get();
                break;
            }
            // nothing more is expected from the client: what it sends is read and ignored
            const qint64 n = net::read(descriptor, buffer.data(), buffer.size());
            bool gone = n == -2;
            if (n == 0 && !ended) {
                ended = true;
                if (!http10 && net::send(descriptor, InterimContinue, sizeof(InterimContinue) - 1) == -2) {
                    gone = true;
                }
            }
            if (gone) {
                job->clientGone = true;
                job->drop(); // still queued: it never runs
                return;      // reset
            }
            if (job->clientDeadline.hasExpired() && job->drop()) {
                // still queued at the client's deadline: answered now, and it never runs
                response = waitedPastDeadline();
                break;
            }
            if (m_stop.load()) {
                // the dispatcher answers 503 at once; do not wait for ever
                if (stopping.isForever()) {
                    stopping = QDeadlineTimer(StopWaitMs);
                }
                else if (stopping.hasExpired()) {
                    return; // reset
                }
            }
        }
    }

    if (!writeAll(descriptor, LC_RemoteApiHttp::serializeResponse(response))) {
        return; // reset
    }
    net::shutdownSending(descriptor);
    // what the client sent meanwhile is read, so that closing does not reset the connection
    while (net::read(descriptor, buffer.data(), buffer.size()) > 0) {
    }
    connection.close();
}

bool LC_RemoteApiServer::writeAll(const qintptr descriptor, const QByteArray& data) const {
    QDeadlineTimer deadline(m_limits.writeTimeoutMs);
    bool stopping = false;
    qint64 written = 0;
    while (written < data.size()) {
        if (m_stop.load() && !stopping) {
            // still deliver this response (often the 503 of the stop itself),
            // but do not hold the stop up for a slow reader
            stopping = true;
            if (deadline.remainingTime() > StopFlushMs || deadline.isForever()) {
                deadline = QDeadlineTimer(StopFlushMs);
            }
        }
        const qint64 n = net::send(descriptor, data.constData() + written, data.size() - written);
        if (n > 0) {
            written += n;
            continue;
        }
        if (n == -2 || deadline.hasExpired()) {
            return false;
        }
        net::waitFor(descriptor, true, static_cast<int>(qMin<qint64>(PollSliceMs, qMax<qint64>(1, deadline.remainingTime()))));
    }
    return true;
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
