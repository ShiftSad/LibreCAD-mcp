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

#include <algorithm>
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
constexpr int AcceptSliceMs = 200;    // how often the accepting thread looks at the stop flag
constexpr int DrainSliceMs = 20;      // the accept loop's slice while refused connections drain
constexpr int BindWaitMs = 2000;
constexpr int StopWaitMs = 2000;
constexpr int DrainMs = 1000;         // reading what is left of a refused request
constexpr std::size_t MaxDraining = 64;
// per connection and pass; the accepting thread wakes as soon as a refused
// client sends more, so this bounds one pass, not the rate
constexpr qint64 DrainReadBudget = 1024 * 1024;
constexpr int StopFlushMs = 300;      // writing the last response while stopping
constexpr int WatchSliceMs = 20;      // looking at the client while its request waits
constexpr qint64 ReadChunk = 64 * 1024;

// Sent to an HTTP/1.1 client that ends its sending while its request waits,
// and again every interimRepeatMs: a client that closed its socket answers it
// with a reset (it is gone), one that only half-closed reads it before the
// final answer (RFC 9110 15.2: a client takes any number of 1xx answers). One
// that closes only later, after reading the first one, answers the next.
const QByteArray InterimContinue = QByteArrayLiteral("HTTP/1.1 100 Continue\r\n\r\n");

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

// What waits to be read, without taking it: > 0 bytes are there; 0 the client
// ended its sending; -1 nothing yet; -2 the connection failed.
qint64 peek(const qintptr descriptor) {
    char byte = 0;
#ifdef _WIN32
    const int n = ::recv(native(descriptor), &byte, 1, MSG_PEEK);
    if (n >= 0) {
        return n;
    }
    return wouldBlock() ? -1 : -2;
#else
    for (;;) {
        const ssize_t n = ::recv(native(descriptor), &byte, 1, MSG_PEEK);
        if (n >= 0) {
            return n;
        }
        if (errno != EINTR) {
            return wouldBlock() ? -1 : -2;
        }
    }
#endif
}

#ifdef _WIN32
using PollEntry = WSAPOLLFD;
#else
using PollEntry = pollfd;
#endif

PollEntry pollEntry(const qintptr descriptor) {
    PollEntry entry{};
    entry.fd = native(descriptor);
    entry.events = POLLIN;
    return entry;
}

// Waits up to ms until one of the sockets can be read (or accepts a
// connection, or failed); the result is in each entry's revents. < 0 on an error.
int poll(std::vector<PollEntry>& entries, const int ms) {
#ifdef _WIN32
    return WSAPoll(entries.data(), static_cast<ULONG>(entries.size()), ms);
#else
    for (;;) {
        const int n = ::poll(entries.data(), static_cast<nfds_t>(entries.size()), ms);
        if (n >= 0 || errno != EINTR) {
            return n;
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

// A reader's connection (already non-blocking): reset when it goes out of
// scope, unless it was closed or handed on.
class Connection {
public:
    explicit Connection(const qintptr descriptor)
        : m_descriptor(descriptor) {
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

// The client's own deadline (X-Request-Timeout-Ms), counted from now: none
// when the header is absent; nullopt when it is not one whole number of
// milliseconds from 1 to MaxClientTimeoutMs (a repeated header arrives joined
// as "700, 700" and is refused too). A client that believes it capped its wait
// must not wait without bound.
std::optional<QDeadlineTimer> parseClientDeadline(const LC_ApiRequest& request) {
    const auto header = request.headers.constFind(QByteArrayLiteral("x-request-timeout-ms"));
    if (header == request.headers.cend()) {
        return QDeadlineTimer(QDeadlineTimer::Forever);
    }
    const QByteArray value = header.value().trimmed(); // the optional whitespace around a field value
    if (value.isEmpty() || value.size() > 9) {
        return std::nullopt;
    }
    qint64 ms = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return std::nullopt; // no sign, fraction, exponent or hex
        }
        ms = ms * 10 + (c - '0');
    }
    if (ms < 1 || ms > LC_RemoteApiServer::MaxClientTimeoutMs) {
        return std::nullopt;
    }
    return QDeadlineTimer(ms);
}

LC_ApiResponse badDeadlineHeader(const LC_ApiRequest& request) {
    QByteArray shown = request.headers.value(QByteArrayLiteral("x-request-timeout-ms"));
    if (shown.size() > 40) {
        shown = shown.left(40) + "...";
    }
    return LC_ApiError(400, QStringLiteral("bad_header"),
                       QStringLiteral("X-Request-Timeout-Ms must be one whole number of milliseconds from 1 to %1, not '%2'")
                           .arg(LC_RemoteApiServer::MaxClientTimeoutMs)
                           .arg(QString::fromLatin1(shown)),
                       {{QStringLiteral("header"), QStringLiteral("X-Request-Timeout-Ms")},
                        {QStringLiteral("min"), 1},
                        {QStringLiteral("max"), LC_RemoteApiServer::MaxClientTimeoutMs}})
        .toResponse();
}

LC_ApiResponse shuttingDown() {
    return LC_ApiError(503, QStringLiteral("shutting_down"), QStringLiteral("the Remote API is stopping")).toResponse();
}

LC_ApiResponse serverBusy() {
    return LC_ApiError(503, QStringLiteral("server_busy"), QStringLiteral("too many connections to the Remote API at once"),
                       {{QStringLiteral("hint"), QStringLiteral("retry in a moment")}})
        .toResponse();
}

LC_ApiResponse headersTooSlow(const int timeoutMs) {
    return LC_ApiError(408, QStringLiteral("request_timeout"),
                       QStringLiteral("the request headers were not received within %1 s").arg(timeoutMs / 1000.0))
        .toResponse();
}

LC_ApiResponse evicted() {
    return LC_ApiError(408, QStringLiteral("request_timeout"),
                       QStringLiteral("the request headers had not arrived when the Remote API needed this connection "
                                      "for another client"))
        .toResponse();
}

// RFC 9110 10.1.1: the members of an Expect header other than 100-continue
// (compared case-insensitively), which the server cannot meet.
QByteArray unmetExpectation(const LC_ApiRequest& request) {
    const auto header = request.headers.constFind(QByteArrayLiteral("expect"));
    if (header == request.headers.cend()) {
        return {};
    }
    for (const QByteArray& member : header.value().split(',')) { // a repeated header arrives joined
        const QByteArray expectation = member.trimmed();
        if (!expectation.isEmpty() && expectation.toLower() != "100-continue") {
            return expectation;
        }
    }
    return {};
}

bool expectsContinue(const LC_ApiRequest& request) {
    const auto header = request.headers.constFind(QByteArrayLiteral("expect"));
    return header != request.headers.cend() && header.value().toLower().contains("100-continue");
}

LC_ApiResponse expectationFailed(const QByteArray& expectation) {
    QByteArray shown = expectation;
    if (shown.size() > 40) {
        shown = shown.left(40) + "...";
    }
    return LC_ApiError(417, QStringLiteral("expectation_failed"),
                       QStringLiteral("the Remote API meets only the expectation 100-continue, not '%1'")
                           .arg(QString::fromLatin1(shown)),
                       {{QStringLiteral("header"), QStringLiteral("Expect")}})
        .toResponse();
}

// Reads what a refused client sent, up to DrainReadBudget: false once the
// client ended its sending or the connection failed. readSome: it sent more.
bool drainSome(const qintptr descriptor, QByteArray& buffer, bool* readSome) {
    for (qint64 budget = DrainReadBudget; budget > 0;) {
        const qint64 n = net::read(descriptor, buffer.data(), buffer.size());
        if (n > 0) {
            budget -= n;
            *readSome = true;
            continue;
        }
        return n == -1;
    }
    return true;
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

// A connection whose request headers have not all arrived: the accepting
// thread reads it.
struct LC_RemoteApiServer::Incoming {
    qintptr descriptor = 0;
    std::shared_ptr<LC_HttpRequestParser> parser;
    QDeadlineTimer headerDeadline;
    QDeadlineTimer readDeadline;   // the whole request, from the connection
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

    // This thread reads every connection until its headers are in: a connection
    // that sends nothing, or sends slowly, holds no thread, and a request past
    // its headers (it has the token) never gives way to one. When maxIncoming
    // connections are still sending their headers, the oldest of them that has
    // nothing waiting to be read is closed (408) for a new one.
    Readers readers;
    std::list<Incoming> incoming; // in the order they arrived
    const std::size_t maxIncoming = static_cast<std::size_t>(qMax(1, m_limits.maxIncoming));
    const int headerTimeoutMs = qMin(m_limits.headerTimeoutMs, m_limits.readTimeoutMs);
    const qintptr listener = server.socketDescriptor();
    QByteArray buffer(ReadChunk, Qt::Uninitialized);
    std::vector<net::PollEntry> polled;
    const auto makeRoom = [this, &incoming] {
        for (auto it = incoming.begin(); it != incoming.end(); ++it) { // oldest first
            const qint64 waiting = net::peek(it->descriptor);
            if (waiting > 0) {
                continue; // its bytes are here: they are read on the next pass
            }
            if (waiting == -1) {
                refuseConnection(it->descriptor, evicted(), !it->parser->isEmpty());
            }
            else {
                net::abort(it->descriptor); // it closed or failed
            }
            incoming.erase(it);
            return true;
        }
        return false;
    };
    while (!m_stop.load()) {
        polled.clear();
        polled.push_back(net::pollEntry(listener));
        qint64 waitMs = AcceptSliceMs;
        for (const Incoming& connection : incoming) {
            polled.push_back(net::pollEntry(connection.descriptor));
            waitMs = qMin(waitMs, qMax<qint64>(0, connection.headerDeadline.remainingTime()));
        }
        {
            // the refused connections too: what their clients still send is read
            // as it arrives, not once per slice (only this thread closes them)
            const std::lock_guard<std::mutex> lock(m_drainMutex);
            for (const Draining& draining : m_draining) {
                polled.push_back(net::pollEntry(draining.descriptor));
            }
            if (!m_draining.empty()) {
                waitMs = qMin<qint64>(waitMs, DrainSliceMs);
            }
        }
        if (net::poll(polled, static_cast<int>(waitMs)) < 0) {
            msleep(PollSliceMs); // a socket error: do not spin, and look at every connection
            for (net::PollEntry& entry : polled) {
                entry.revents = POLLIN;
            }
        }
        readers.remove_if([](const std::unique_ptr<QThread>& reader) {
            return reader->isFinished();
        });

        // what the connections still sending their headers sent (polled in this order)
        std::size_t index = 1;
        for (auto it = incoming.begin(); it != incoming.end(); ++index) {
            bool left = false;
            if (polled.at(index).revents != 0) {
                left = readIncoming(*it, buffer, readers);
            }
            if (!left && it->headerDeadline.hasExpired()) {
                refuseConnection(it->descriptor, headersTooSlow(headerTimeoutMs), !it->parser->isEmpty());
                left = true;
            }
            it = left ? incoming.erase(it) : std::next(it);
        }

        bool timedOut = false;
        server.waitForNewConnection(0, &timedOut);
        for (const qintptr descriptor : server.takePending()) {
            net::prepare(descriptor);
            if (m_stop.load()) {
                refuseConnection(descriptor, shuttingDown(), true);
                continue;
            }
            if (incoming.size() >= maxIncoming && !makeRoom()) {
                refuseConnection(descriptor, serverBusy(), true);
                continue;
            }
            incoming.push_back({descriptor,
                                std::make_shared<LC_HttpRequestParser>(m_limits.maxHeaderBytes, m_limits.maxBodyBytes),
                                QDeadlineTimer(headerTimeoutMs), QDeadlineTimer(m_limits.readTimeoutMs)});
        }
        serviceDraining(false);
    }
    m_listening = false;
    server.close();
    for (const qintptr descriptor : server.takePending()) {
        net::prepare(descriptor);
        refuseConnection(descriptor, shuttingDown(), true);
    }
    for (const Incoming& connection : incoming) {
        net::abort(connection.descriptor); // no request yet: reset
    }
    incoming.clear();
    // The dispatcher answers what is still queued (503) before it ends; the
    // readers write those answers and end too.
    m_queueChanged.notify_all();
    dispatcher->wait();
    for (const std::unique_ptr<QThread>& reader : readers) {
        reader->wait();
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

bool LC_RemoteApiServer::readIncoming(Incoming& incoming, QByteArray& buffer, Readers& readers) {
    LC_HttpRequestParser& parser = *incoming.parser;
    for (;;) {
        const qint64 n = net::read(incoming.descriptor, buffer.data(), buffer.size());
        if (n > 0) {
            parser.feed(QByteArrayView(buffer.constData(), n));
            if (parser.headersComplete() || parser.state() != LC_HttpRequestParser::State::Incomplete) {
                break; // the body is the reader's
            }
            continue;
        }
        if (n == 0) { // the client ended its sending
            if (parser.isEmpty()) {
                net::close(incoming.descriptor); // opened and closed without a request
                return true;
            }
            parser.finish();
            break;
        }
        if (n == -2) {
            net::abort(incoming.descriptor); // the connection failed
            return true;
        }
        return false; // nothing more yet
    }
    if (parser.state() == LC_HttpRequestParser::State::Error) {
        refuseConnection(incoming.descriptor, parser.error().toResponse(), true);
        return true;
    }
    if (!parser.headersComplete()) {
        return false;
    }
    admit(incoming, readers);
    return true;
}

void LC_RemoteApiServer::admit(Incoming& incoming, Readers& readers) {
    // before the body: a client without the token never gets to send it
    QDeadlineTimer deadline(QDeadlineTimer::Forever);
    const bool bodyComing = incoming.parser->state() == LC_HttpRequestParser::State::Incomplete;
    if (const std::optional<LC_ApiResponse> refused = refusal(incoming.parser->request(), &deadline)) {
        refuseConnection(incoming.descriptor, *refused, bodyComing);
        return;
    }
    if (static_cast<int>(readers.size()) >= qMax(1, m_limits.maxConnections)) {
        refuseConnection(incoming.descriptor, serverBusy(), bodyComing);
        return;
    }
    std::unique_ptr<QThread> thread(QThread::create(
        [this, descriptor = incoming.descriptor, parser = incoming.parser, readDeadline = incoming.readDeadline, deadline] {
            serveConnection(descriptor, parser, readDeadline, deadline);
        }));
    thread->setObjectName(QStringLiteral("remote-api-connection"));
    thread->start();
    readers.push_back(std::move(thread));
}

void LC_RemoteApiServer::refuseConnection(const qintptr descriptor, const LC_ApiResponse& response, const bool sending) {
    // one try: the answer is small and nothing else was sent on the socket; the
    // accepting thread never waits for a client
    const QByteArray data = LC_RemoteApiHttp::serializeResponse(response);
    net::send(descriptor, data.constData(), data.size());
    net::shutdownSending(descriptor);
    drainLater(descriptor, sending);
}

void LC_RemoteApiServer::refuseAndDrain(const qintptr descriptor, const LC_ApiResponse& response) {
    if (!writeAll(descriptor, LC_RemoteApiHttp::serializeResponse(response))) {
        net::abort(descriptor);
        return;
    }
    net::shutdownSending(descriptor);
    drainLater(descriptor, true); // a reader refuses while the body comes
}

void LC_RemoteApiServer::drainLater(const qintptr descriptor, const bool sending) {
    // the accepting thread keeps the list within MaxDraining (serviceDraining)
    const std::lock_guard<std::mutex> lock(m_drainMutex);
    m_draining.push_back({descriptor, QDeadlineTimer(DrainMs), sending});
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
    for (Draining& draining : current) {
        bool readSome = false;
        const bool open = drainSome(draining.descriptor, buffer, &readSome);
        draining.sending = draining.sending || readSome;
        if (!open || closeAll || draining.until.hasExpired()) {
            net::close(draining.descriptor); // ended, failed, or its second is over
        }
        else {
            keep.push_back(draining);
        }
    }
    // Too many at once: closing one whose client still sends would reset its
    // connection, and the client would lose the answer it has not read yet. So
    // the oldest ones whose client is not expected to send more (its request
    // was complete, or it sent nothing) go first; one that still sends goes
    // only when no other is left.
    std::vector<Draining> overflow;
    {
        const std::lock_guard<std::mutex> lock(m_drainMutex);
        m_draining.insert(m_draining.begin(), keep.cbegin(), keep.cend()); // the oldest first
        std::size_t excess = m_draining.size() > MaxDraining ? m_draining.size() - MaxDraining : 0;
        for (const bool takeSending : {false, true}) {
            for (auto it = m_draining.begin(); it != m_draining.end() && excess > 0;) {
                if (takeSending || !it->sending) {
                    overflow.push_back(*it);
                    it = m_draining.erase(it);
                    --excess;
                }
                else {
                    ++it;
                }
            }
        }
    }
    for (const Draining& closing : overflow) {
        bool readSome = false;
        drainSome(closing.descriptor, buffer, &readSome); // what is left unread would reset it
        net::close(closing.descriptor);
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

void LC_RemoteApiServer::forget(const std::shared_ptr<Job>& job) {
    {
        const std::lock_guard<std::mutex> lock(m_queueMutex);
        const auto it = std::find(m_queue.begin(), m_queue.end(), job);
        if (it != m_queue.end()) {
            m_queue.erase(it);
        }
    }
    // Dropped, so the dispatcher no longer reads it (it skips a job it could not
    // take): the body is freed now, not when the dispatcher gets to the job
    // behind a request that may hold it for minutes.
    job->request = LC_ApiRequest();
}

qint64 LC_RemoteApiServer::queuedBodyBytes() const {
    const std::lock_guard<std::mutex> lock(m_queueMutex);
    qint64 bytes = 0;
    for (const std::shared_ptr<Job>& job : m_queue) {
        bytes += job->request.body.size();
    }
    return bytes;
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

void LC_RemoteApiServer::serveConnection(const qintptr descriptor, const std::shared_ptr<LC_HttpRequestParser>& parser,
                                         const QDeadlineTimer readDeadline, const QDeadlineTimer clientDeadline) {
    Connection connection(descriptor);
    QByteArray buffer(ReadChunk, Qt::Uninitialized);
    // RFC 9110 10.1.1: a client that sent "Expect: 100-continue" waits for an
    // interim 100 (or the final answer) before it sends its body; curl waits a
    // second for it before every body over 1 MiB. HTTP/1.0 ignores it.
    if (parser->state() == LC_HttpRequestParser::State::Incomplete && !parser->isHttp10() &&
        expectsContinue(parser->request()) && !writeAll(descriptor, InterimContinue)) {
        return; // reset
    }
    bool timedOut = false;
    while (parser->state() == LC_HttpRequestParser::State::Incomplete) { // the body
        if (m_stop.load()) {
            return; // reset
        }
        const qint64 n = net::read(descriptor, buffer.data(), buffer.size());
        if (n > 0) {
            parser->feed(QByteArrayView(buffer.constData(), n));
            continue;
        }
        if (n == 0) { // the client ended its sending
            parser->finish();
            break;
        }
        if (n == -2) {
            return; // the connection failed
        }
        if (readDeadline.hasExpired()) {
            timedOut = true;
            break;
        }
        net::waitFor(descriptor, false, static_cast<int>(qMin<qint64>(PollSliceMs, qMax<qint64>(1, readDeadline.remainingTime()))));
    }
    if (timedOut || parser->state() == LC_HttpRequestParser::State::Error) {
        const LC_ApiResponse refused =
            timedOut ? LC_ApiError(408, QStringLiteral("request_timeout"),
                                   QStringLiteral("the request was not received within %1 s").arg(m_limits.readTimeoutMs / 1000.0))
                           .toResponse()
                     : parser->error().toResponse();
        refuseAndDrain(connection.release(), refused);
        return;
    }

    const bool http10 = parser->isHttp10();
    auto job = std::make_shared<Job>();
    job->request = parser->takeRequest();
    job->clientDeadline = clientDeadline;
    std::future<LC_ApiResponse> answer = job->promise.get_future();
    LC_ApiResponse response;
    QByteArray interim; // what is left to send of an interim answer
    if (!enqueue(job)) {
        response = shuttingDown();
    }
    else {
        // Watch the client while the request waits or runs (see the class comment).
        bool ended = false; // the client ended its sending
        QDeadlineTimer nextInterim(QDeadlineTimer::Forever);
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
                if (!http10) {
                    nextInterim = QDeadlineTimer(0);
                }
            }
            if (!gone && nextInterim.hasExpired()) {
                if (interim.isEmpty()) {
                    interim = InterimContinue;
                }
                const qint64 sent = net::send(descriptor, interim.constData(), interim.size());
                if (sent == -2) {
                    gone = true;
                }
                else if (sent > 0) {
                    interim.remove(0, sent);
                }
                nextInterim = QDeadlineTimer(qMax(1, m_limits.interimRepeatMs));
            }
            if (gone) {
                job->clientGone = true;
                if (job->drop()) { // still queued: it never runs
                    forget(job);
                }
                return; // reset
            }
            if (job->clientDeadline.hasExpired() && job->drop()) {
                // still queued at the client's deadline: answered now, and it never runs
                forget(job);
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

    // an interim answer cut short is completed first
    if (!writeAll(descriptor, interim + LC_RemoteApiHttp::serializeResponse(response))) {
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

std::optional<LC_ApiResponse> LC_RemoteApiServer::refusal(const LC_ApiRequest& request, QDeadlineTimer* clientDeadline) const {
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
    const std::optional<QDeadlineTimer> deadline = parseClientDeadline(request);
    if (!deadline.has_value()) {
        return badDeadlineHeader(request);
    }
    if (const QByteArray unmet = unmetExpectation(request); !unmet.isEmpty()) {
        return expectationFailed(unmet);
    }
    if (clientDeadline != nullptr) {
        *clientDeadline = *deadline;
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
