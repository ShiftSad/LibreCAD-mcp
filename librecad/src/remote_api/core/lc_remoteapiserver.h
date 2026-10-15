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

#ifndef LC_REMOTEAPISERVER_H
#define LC_REMOTEAPISERVER_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <optional>

#include <vector>

#include <QByteArray>
#include <QDeadlineTimer>
#include <QString>
#include <QThread>

#include "lc_remoteapitypes.h"

class LC_HttpRequestParser;
class LC_RemoteApiContext;
class LC_RemoteApiRouter;

/**
 * The HTTP transport of the Remote API (design sections 1.2 and 3.3).
 *
 * A QThread named "remote-api" that listens on 127.0.0.1 only. It reads every
 * connection itself until the request headers are in, so a client that is slow
 * to send (or sends nothing) holds no thread; a request past its headers and
 * the checks gets a reader thread of its own, and requests are still handled
 * one at a time, in the order they arrive, by a single dispatcher thread. For
 * each connection, in order:
 *  1. read the request line and headers within the header timeout (408), with
 *     the header limit (431); chunked bodies are refused (411);
 *  2. check Host (421 bad_host), then Origin (403 origin_not_allowed), then
 *     the token, X-Api-Token or Authorization: Bearer (401 unauthorized), then
 *     X-Request-Timeout-Ms (400 bad_header), then Expect (417
 *     expectation_failed for anything but 100-continue), as soon as the
 *     headers are in: a refused client never sends its body;
 *  3. read the body within the read timeout (408), with the body limit (413);
 *     an HTTP/1.1 client that sent "Expect: 100-continue" is sent
 *     "100 Continue" first, unless its body is already in;
 *  4. queue it for the dispatcher, which calls router.dispatch() (GUI routes
 *     run on the GUI thread). While it waits, the reader watches the client:
 *     - X-Request-Timeout-Ms (the client's own deadline, in ms) caps the wait:
 *       a request still queued then is answered 504 ui_timeout (started:
 *       false) at once and never runs; one that runs stops waiting for the
 *       GUI thread at that deadline;
 *     - a client whose connection fails is gone: a queued request is dropped
 *       without running, a running one stops waiting for the GUI thread;
 *     - a dropped request leaves the queue at once, with its body;
 *     - a client that ends its sending (a half-close, or a full close) may still
 *       read: an HTTP/1.1 client is sent "100 Continue", and again every
 *       interimRepeatMs while it waits, which a closed socket answers with a
 *       reset (the client is gone), while a half-closed one simply reads them
 *       before the final answer; an HTTP/1.0 client is sent nothing, so its
 *       end of sending counts as a half-close;
 *  5. write the response and close the connection.
 * At most maxConnections requests are past their headers at the same time; one
 * more is answered 503 server_busy. At most maxIncoming connections may still
 * be sending their headers; when one more arrives, the oldest of them that has
 * nothing waiting to be read is closed (408) to make room. Refused connections
 * are read for a short while before they close, so that the client reads the
 * refusal instead of a reset.
 *
 * The router and the context must outlive the server; the server only reads
 * the router (routes are added before start()).
 */
class LC_RemoteApiServer : public QThread {
public:
    struct Limits {
        qsizetype maxHeaderBytes = 16 * 1024;
        qsizetype maxBodyBytes = 4 * 1024 * 1024;
        int headerTimeoutMs = 5000;    // request line and headers, from the connection
        int readTimeoutMs = 15000;     // the whole request, from the connection
        int writeTimeoutMs = 15000;
        int maxConnections = 16;       // requests past their headers at the same time
        int maxIncoming = 64;          // connections still sending their headers
        int interimRepeatMs = 1000;    // "100 Continue" again, to a client that ended its sending
    };

    enum class StartResult { Listening, PortInUse, Failed };

    // Largest X-Request-Timeout-Ms taken into account (10 minutes).
    static constexpr qint64 MaxClientTimeoutMs = 600000;

    LC_RemoteApiServer(const LC_RemoteApiRouter& router, LC_RemoteApiContext& context);
    ~LC_RemoteApiServer() override;
    LC_RemoteApiServer(const LC_RemoteApiServer&) = delete;
    LC_RemoteApiServer& operator=(const LC_RemoteApiServer&) = delete;

    // Before start(). Tests shorten the timeouts.
    void setLimits(const Limits& limits) { m_limits = limits; }

    // Binds 127.0.0.1:port (0 picks a free port) and starts the thread. Waits up
    // to 2 s for the bind result. An empty token fails closed. On failure the
    // thread is not running and error describes why.
    StartResult start(quint16 port, const QString& token, QString* error = nullptr);
    // Idempotent. Every wait on the GUI thread is aborted (503 shutting_down),
    // queued requests are answered 503 without running, and the threads end
    // within about 200 ms.
    void stop();

    bool isListening() const { return m_listening.load(); }
    quint16 port() const { return m_port.load(); }

    // Steps 2 and 4 for one parsed request (also used by the tests).
    LC_ApiResponse handle(const LC_ApiRequest& request) const;
    // The bytes of request bodies waiting for the dispatcher (for the tests).
    qint64 queuedBodyBytes() const;

protected:
    void run() override;

private:
    struct Job;
    struct Incoming;
    using Readers = std::list<std::unique_ptr<QThread>>;

    // A reader thread: the rest of the request, the dispatcher's answer, the response.
    void serveConnection(qintptr descriptor, const std::shared_ptr<LC_HttpRequestParser>& parser,
                         QDeadlineTimer readDeadline, QDeadlineTimer clientDeadline);
    void dispatchLoop();                        // the dispatcher thread
    // The accepting thread: reads what a connection still sending its headers
    // sent; true when it left the accepting thread (handed to a reader, refused
    // or closed).
    bool readIncoming(Incoming& incoming, QByteArray& buffer, Readers& readers);
    // Steps 2 and the reader's start, for a connection whose headers are in.
    void admit(Incoming& incoming, Readers& readers);
    // Answers a connection on the accepting thread (one try: the answer is small).
    void refuseConnection(qintptr descriptor, const LC_ApiResponse& response);
    // Writes a refusal, then hands the connection to the accepting thread.
    void refuseAndDrain(qintptr descriptor, const LC_ApiResponse& response);
    // The accepting thread reads what the client still sends for a short while,
    // then closes the connection: closing it with unread data would reset it,
    // and the client would lose the answer.
    void drainLater(qintptr descriptor);
    void serviceDraining(bool closeAll);       // the accepting thread
    bool enqueue(const std::shared_ptr<Job>& job);
    // A reader that dropped its job takes it out of the queue, with its body.
    void forget(const std::shared_ptr<Job>& job);
    bool writeAll(qintptr descriptor, const QByteArray& data) const;
    bool hostAllowed(const QByteArray& host) const;
    // Step 2: the refusal of a request that may not reach the router, if any;
    // otherwise clientDeadline (when given) gets its X-Request-Timeout-Ms.
    std::optional<LC_ApiResponse> refusal(const LC_ApiRequest& request, QDeadlineTimer* clientDeadline = nullptr) const;
    // Step 4 with every exception turned into an envelope.
    LC_ApiResponse dispatchSafely(const LC_ApiRequest& request) const;

    const LC_RemoteApiRouter& m_router;
    LC_RemoteApiContext& m_context;
    Limits m_limits;
    QByteArray m_token;
    quint16 m_requestedPort = 0;
    // LC_RemoteApiGui's abort generation when this run started: a stop()
    // changes it, and every wait of this run then ends (it is sticky, so a
    // stop that comes between a request and its post is not missed).
    quint64 m_abortGeneration = 0;

    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_listening{false};
    std::atomic<quint16> m_port{0};

    // requests waiting for the dispatcher
    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueChanged;
    std::deque<std::shared_ptr<Job>> m_queue;

    // refused connections, read for a short while before they close
    struct Draining {
        qintptr descriptor = 0;
        QDeadlineTimer until;
    };
    std::mutex m_drainMutex;
    std::vector<Draining> m_draining;

    // the bind result, handed from run() to start()
    struct BindOutcome {
        StartResult result = StartResult::Failed;
        QString error;
    };
    std::shared_ptr<std::promise<BindOutcome>> m_bindPromise;
};

#endif
