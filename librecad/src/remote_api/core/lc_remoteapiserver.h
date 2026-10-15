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
#include <memory>
#include <mutex>
#include <optional>

#include <QByteArray>
#include <QString>
#include <QThread>

#include "lc_remoteapitypes.h"

class LC_RemoteApiContext;
class LC_RemoteApiRouter;
class QTcpSocket;

/**
 * The HTTP transport of the Remote API (design sections 1.2 and 3.3).
 *
 * A QThread named "remote-api" that listens on 127.0.0.1 only. Each accepted
 * connection gets a reader thread of its own, so a client that is slow to send
 * (or sends nothing) holds up nobody but itself; requests are still handled one
 * at a time, in the order they arrive, by a single dispatcher thread. For each
 * connection, in order:
 *  1. read and parse the request: the request line and headers within the
 *     header timeout, everything within the read timeout (408), with the
 *     header (431) and body (413) limits; chunked bodies are refused (411);
 *  2. check Host (421 bad_host), then Origin (403 origin_not_allowed), then
 *     the token, X-Api-Token or Authorization: Bearer (401 unauthorized);
 *  3. queue it for the dispatcher, which calls router.dispatch() (GUI routes
 *     run on the GUI thread). A request whose client has closed the connection
 *     by then is dropped without running, and X-Request-Timeout-Ms (the
 *     client's own deadline, in ms) caps how long it may wait for LibreCAD;
 *  4. write the response and close the connection.
 * At most maxConnections connections are read at the same time; one more is
 * answered 503 server_busy at once.
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
        int maxConnections = 16;       // read at the same time
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

    // Steps 2 and 3 for one parsed request (also used by the tests).
    LC_ApiResponse handle(const LC_ApiRequest& request) const;

protected:
    void run() override;

private:
    struct Job;

    void serveConnection(qintptr descriptor);   // a reader thread
    void dispatchLoop();                        // the dispatcher thread
    void refuseBusy(qintptr descriptor) const;  // the accepting thread
    bool enqueue(const std::shared_ptr<Job>& job);
    bool writeAll(QTcpSocket& socket, const QByteArray& data) const;
    void drainAndClose(QTcpSocket& socket) const;
    bool hostAllowed(const QByteArray& host) const;
    // Step 2: the refusal of a request that may not reach the router, if any.
    std::optional<LC_ApiResponse> refusal(const LC_ApiRequest& request) const;
    // Step 3 with every exception turned into an envelope.
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
    std::mutex m_queueMutex;
    std::condition_variable m_queueChanged;
    std::deque<std::shared_ptr<Job>> m_queue;

    // the bind result, handed from run() to start()
    struct BindOutcome {
        StartResult result = StartResult::Failed;
        QString error;
    };
    std::shared_ptr<std::promise<BindOutcome>> m_bindPromise;
};

#endif
