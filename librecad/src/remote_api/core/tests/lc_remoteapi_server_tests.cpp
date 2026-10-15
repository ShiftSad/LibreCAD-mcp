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

// The Remote API over real loopback sockets (design section 5.1, "Server"):
// authentication, Host and Origin checks, limits, GUI routes, binding, stop(),
// and the controller with its discovery file and /status, without a main window.

// First: a client here half-closes its connection through the native API.
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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include "lc_actiontestsupport.h"
#include "lc_remoteapiconfig.h"
#include "lc_remoteapicontext.h"
#include "lc_remoteapicontroller.h"
#include "lc_remoteapigui.h"
#include "lc_remoteapirouter.h"
#include "lc_remoteapiserver.h"

namespace {

struct HttpReply {
    bool connected = false;
    int status = 0;
    QByteArray head;
    QByteArray body;
    QJsonObject json;

    QByteArray header(const QByteArray& name) const {
        for (const QByteArray& line : head.split('\n')) {
            const qsizetype colon = line.indexOf(':');
            if (colon > 0 && line.left(colon).trimmed().toLower() == name.toLower()) {
                return line.mid(colon + 1).trimmed();
            }
        }
        return {};
    }
    QString error() const { return json.value("error").toString(); }
};

// Ends the sending side of a client's connection (shutdown(SHUT_WR)): it can still read.
void halfClose(QTcpSocket& socket) {
#ifdef _WIN32
    ::shutdown(static_cast<SOCKET>(socket.socketDescriptor()), SD_SEND);
#else
    ::shutdown(static_cast<int>(socket.socketDescriptor()), SHUT_WR);
#endif
}

// A plain blocking client socket, for floods and for closing exactly when a test says.
#ifdef _WIN32
using NativeSocket = SOCKET;
const NativeSocket NoSocket = INVALID_SOCKET;
void closeNative(const NativeSocket s) {
    ::closesocket(s);
}
#else
using NativeSocket = int;
const NativeSocket NoSocket = -1;
void closeNative(const NativeSocket s) {
    ::close(s);
}
#endif

NativeSocket connectNative(const quint16 port, const int receiveTimeoutMs = 5000) {
#ifdef _WIN32
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
    const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == NoSocket) {
        return NoSocket;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closeNative(s);
        return NoSocket;
    }
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(receiveTimeoutMs);
#else
    timeval timeout{receiveTimeoutMs / 1000, (receiveTimeoutMs % 1000) * 1000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return s;
}

bool sendNative(const NativeSocket s, const QByteArray& data) {
    qsizetype sent = 0;
    while (sent < data.size()) {
        const auto n = ::send(s, data.constData() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) {
            return false;
        }
        sent += n;
    }
    return true;
}

// Reads until the data holds until (or the connection ends, or the receive timeout passes).
QByteArray receiveNative(const NativeSocket s, const QByteArray& until) {
    QByteArray data;
    char chunk[4096];
    while (!data.contains(until)) {
        const auto n = ::recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            break;
        }
        data.append(chunk, n);
    }
    return data;
}

void halfCloseNative(const NativeSocket s) {
#ifdef _WIN32
    ::shutdown(s, SD_SEND);
#else
    ::shutdown(s, SHUT_WR);
#endif
}

// Reads until the server closes the connection (or the receive timeout passes).
QByteArray receiveUntilClosed(const NativeSocket s) {
    QByteArray data;
    char chunk[4096];
    for (;;) {
        const auto n = ::recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return data;
        }
        data.append(chunk, n);
    }
}

// Whether the server sent something within ms (without reading it).
bool readableWithin(const NativeSocket s, const int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval timeout{ms / 1000, (ms % 1000) * 1000};
    return ::select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &timeout) > 0;
}

// How exchange() behaves, and what it saw.
struct Exchange {
    bool halfClose = false;  // end the sending after the request, as ncat or socat do
    int interim = 0;         // interim 1xx answers before the final one (skipped)
};

// One exchange on a fresh connection: send request (may be empty: a silent
// client), then read until the server closes or timeoutMs passes. Runs on any thread.
HttpReply exchange(const quint16 port, const QByteArray& request, const int timeoutMs = 10000, Exchange* options = nullptr) {
    HttpReply reply;
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(5000)) {
        return reply;
    }
    reply.connected = true;
    if (!request.isEmpty()) {
        socket.write(request);
        QElapsedTimer writing;
        writing.start();
        while (socket.bytesToWrite() > 0 && writing.elapsed() < 5000 &&
               socket.state() == QAbstractSocket::ConnectedState) {
            socket.waitForBytesWritten(100);
        }
        if (options != nullptr && options->halfClose) {
            halfClose(socket);
        }
    }
    QByteArray data;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (socket.bytesAvailable() > 0) {
            data += socket.readAll();
            continue;
        }
        if (socket.state() != QAbstractSocket::ConnectedState) {
            break;
        }
        socket.waitForReadyRead(50);
    }
    data += socket.readAll();
    while (data.startsWith("HTTP/1.1 1")) { // an interim answer: the final one follows
        const qsizetype end = data.indexOf("\r\n\r\n");
        if (end < 0) {
            break;
        }
        data.remove(0, end + 4);
        if (options != nullptr) {
            ++options->interim;
        }
    }
    const qsizetype split = data.indexOf("\r\n\r\n");
    if (split < 0) {
        return reply;
    }
    reply.head = data.left(split);
    reply.body = data.mid(split + 4);
    const QList<QByteArray> statusLine = reply.head.left(reply.head.indexOf("\r\n")).split(' ');
    reply.status = statusLine.size() >= 2 ? statusLine.at(1).toInt() : 0;
    reply.json = QJsonDocument::fromJson(reply.body).object();
    return reply;
}

// Runs work on a std::thread while this (the GUI) thread processes events.
void runWhilePumping(const std::function<void()>& work) {
    std::atomic<bool> done{false};
    std::thread worker([&] {
        work();
        done = true;
    });
    QElapsedTimer timer;
    timer.start();
    while (!done.load() && timer.elapsed() < 30000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    worker.join();
    REQUIRE(done.load());
}

// Lets the GUI thread run what was posted to it, for ms.
void pumpFor(const int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

HttpReply send(const quint16 port, const QByteArray& request, const int timeoutMs = 10000) {
    HttpReply reply;
    runWhilePumping([&] { reply = exchange(port, request, timeoutMs); });
    return reply;
}

QByteArray get(const QByteArray& path, const quint16 port, const QByteArray& token, const QByteArray& extra = {}) {
    QByteArray request = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\n";
    if (!token.isEmpty()) {
        request += "X-Api-Token: " + token + "\r\n";
    }
    return request + extra + "\r\n";
}

quint16 freePort() {
    QTcpServer probe;
    REQUIRE(probe.listen(QHostAddress::LocalHost, 0));
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

// A router with a route served on the server thread, one on the GUI thread
// and a body echo, plus a server on a free port.
struct Fixture {
    LC_RemoteApiContext context{nullptr};
    LC_RemoteApiRouter router;
    QByteArray token = LC_RemoteApiConfig::generateToken().toUtf8();
    std::unique_ptr<LC_RemoteApiServer> server;

    Fixture() {
        (void)lc::test::application();
        LC_RouteOptions direct;
        direct.capability = "test";
        direct.gui = false;
        LC_RouteOptions gui;
        gui.capability = "test";
        router.add("GET", "/api/v1/ping", direct, [](LC_ApiCall&) { return LC_ApiResponse::ok({{"pong", true}}); });
        router.add("GET", "/api/v1/gui", gui, [](LC_ApiCall&) {
            return LC_ApiResponse::ok({{"on_gui_thread", QThread::currentThread() == qApp->thread()}});
        });
        router.add("POST", "/api/v1/echo", direct, [](LC_ApiCall& call) {
            return LC_ApiResponse::ok({{"received", call.body().raw()}}, 201);
        });
        router.add("GET", "/api/v1/items/{name}", direct, [](LC_ApiCall& call) {
            return LC_ApiResponse::ok({{"name", call.params.value("name")}});
        });
        // contradictory options: changing a document always runs on the GUI thread
        LC_RouteOptions changesDirect = direct;
        changesDirect.changesDocument = true;
        router.add("POST", "/api/v1/changes", changesDirect, [](LC_ApiCall& call) {
            call.audit = "[MCP] changed";
            return LC_ApiResponse::ok({{"on_gui_thread", QThread::currentThread() == qApp->thread()}});
        });
        // an audit line from a route on the server thread is written on the GUI thread
        router.add("POST", "/api/v1/audit-direct", direct, [](LC_ApiCall& call) {
            call.audit = "[MCP] audited";
            call.forceAudit = true;
            return LC_ApiResponse::ok({{"on_gui_thread", QThread::currentThread() == qApp->thread()}});
        });
    }

    quint16 start(const LC_RemoteApiServer::Limits& limits = {}) {
        server = std::make_unique<LC_RemoteApiServer>(router, context);
        server->setLimits(limits);
        QString error;
        REQUIRE(server->start(0, QString::fromUtf8(token), &error) == LC_RemoteApiServer::StartResult::Listening);
        REQUIRE(server->isListening());
        REQUIRE(server->port() != 0);
        return server->port();
    }
};

} // namespace

TEST_CASE("Remote API server answers requests that carry the token", "[remote-api][server]") {
    Fixture fixture;
    const quint16 port = fixture.start();

    HttpReply reply = send(port, get("/api/v1/ping", port, fixture.token));
    CHECK(reply.status == 200);
    CHECK(reply.json.value("pong").toBool());
    CHECK(reply.header("Connection") == "close");
    CHECK(reply.header("Cache-Control") == "no-store");
    CHECK(reply.header("Content-Type") == "application/json; charset=utf-8");
    CHECK(reply.header("Content-Length") == QByteArray::number(reply.body.size()));
    CHECK(reply.header("Access-Control-Allow-Origin").isEmpty()); // no CORS

    reply = send(port, get("/api/v1/ping", port, {}, "Authorization: Bearer " + fixture.token + "\r\n"));
    CHECK(reply.status == 200);

    reply = send(port, get("/api/v1/gui", port, fixture.token));
    CHECK(reply.status == 200);
    CHECK(reply.json.value("on_gui_thread").toBool());

    const QByteArray body = "{\"a\":[1,2],\"text\":\"\xc3\xbc\"}"; // u-umlaut in UTF-8
    reply = send(port, "POST /api/v1/echo HTTP/1.1\r\nHost: localhost:" + QByteArray::number(port) +
                           "\r\nX-Api-Token: " + fixture.token + "\r\nContent-Length: " +
                           QByteArray::number(body.size()) + "\r\n\r\n" + body);
    CHECK(reply.status == 201);
    CHECK(reply.json.value("received").toObject().value("text").toString() == QString(QChar(0x00FC)));

    reply = send(port, get("/api/v1/items/walls%2Fold%20one", port, fixture.token));
    CHECK(reply.status == 200);
    CHECK(reply.json.value("name").toString() == "walls/old one");

    reply = send(port, get("/api/v1/nope", port, fixture.token));
    CHECK(reply.status == 404);
    CHECK(reply.error() == "not_found");

    const QByteArray post = "Host: 127.0.0.1:" + QByteArray::number(port) + "\r\nX-Api-Token: " + fixture.token +
                            "\r\nContent-Length: 0\r\n\r\n";
    reply = send(port, "POST /api/v1/changes HTTP/1.1\r\n" + post);
    CHECK(reply.status == 200);
    CHECK(reply.json.value("on_gui_thread").toBool());
    reply = send(port, "POST /api/v1/audit-direct HTTP/1.1\r\n" + post);
    CHECK(reply.status == 200);
    CHECK_FALSE(reply.json.value("on_gui_thread").toBool(true));

    reply = send(port, "OPTIONS /api/v1/ping HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) +
                           "\r\nX-Api-Token: " + fixture.token + "\r\n\r\n");
    CHECK(reply.status == 405);
    CHECK(reply.error() == "method_not_allowed");
    CHECK(reply.header("Allow") == "GET");
}

TEST_CASE("Remote API server refuses requests without the right token", "[remote-api][server]") {
    Fixture fixture;
    const quint16 port = fixture.start();

    HttpReply reply = send(port, get("/api/v1/ping", port, {}));
    CHECK(reply.status == 401);
    CHECK(reply.error() == "unauthorized");
    CHECK(reply.json.value("app").toString() == "LibreCAD");
    CHECK(reply.header("WWW-Authenticate").startsWith("Bearer"));

    reply = send(port, get("/api/v1/ping", port, "0123456789abcdef0123456789abcdef"));
    CHECK(reply.status == 401);
    reply = send(port, get("/api/v1/ping", port, fixture.token.left(31)));
    CHECK(reply.status == 401);
    reply = send(port, get("/api/v1/ping", port, {}, "Authorization: Basic " + fixture.token + "\r\n"));
    CHECK(reply.status == 401);
    // the token is checked before routing: an unknown route without it is 401 too
    reply = send(port, get("/api/v1/nope", port, {}));
    CHECK(reply.status == 401);
    CHECK_FALSE(reply.body.contains(fixture.token)); // never echoed
}

TEST_CASE("Remote API server refuses foreign hosts and browsers", "[remote-api][server]") {
    Fixture fixture;
    const quint16 port = fixture.start();
    const QByteArray portText = QByteArray::number(port);
    const QByteArray token = "X-Api-Token: " + fixture.token + "\r\n";

    HttpReply reply = send(port, "GET /api/v1/ping HTTP/1.1\r\nHost: evil.com\r\n" + token + "\r\n");
    CHECK(reply.status == 421);
    CHECK(reply.error() == "bad_host");
    reply = send(port, "GET /api/v1/ping HTTP/1.1\r\nHost: evil.com:" + portText + "\r\n" + token + "\r\n");
    CHECK(reply.status == 421);
    reply = send(port, "GET /api/v1/ping HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port + 1) + "\r\n" + token + "\r\n");
    CHECK(reply.status == 421);
    reply = send(port, "GET /api/v1/ping HTTP/1.1\r\n" + token + "\r\n");
    CHECK(reply.status == 421);
    reply = send(port, "GET /api/v1/ping HTTP/1.1\r\nHost: LOCALHOST:" + portText + "\r\n" + token + "\r\n");
    CHECK(reply.status == 200);
    reply = send(port, "GET /api/v1/ping HTTP/1.1\r\nHost: [::1]:" + portText + "\r\n" + token + "\r\n");
    CHECK(reply.status == 200);

    reply = send(port, get("/api/v1/ping", port, fixture.token, "Origin: http://example.com\r\n"));
    CHECK(reply.status == 403);
    CHECK(reply.error() == "origin_not_allowed");
    reply = send(port, get("/api/v1/ping", port, fixture.token, "Origin: null\r\n"));
    CHECK(reply.status == 403);
}

TEST_CASE("Remote API server enforces the request limits", "[remote-api][server]") {
    Fixture fixture;
    const quint16 port = fixture.start();
    const QByteArray host = "Host: 127.0.0.1:" + QByteArray::number(port) + "\r\nX-Api-Token: " + fixture.token + "\r\n";

    SECTION("a 5 MiB body is refused from its Content-Length") {
        const HttpReply reply = send(port, "POST /api/v1/echo HTTP/1.1\r\n" + host + "Content-Length: " +
                                               QByteArray::number(5 * 1024 * 1024) + "\r\n\r\n" + QByteArray(1024, '{'));
        CHECK(reply.status == 413);
        CHECK(reply.error() == "body_too_large");
        CHECK(reply.json.value("limit").toInteger() == 4 * 1024 * 1024);
    }
    SECTION("chunked bodies are refused") {
        const HttpReply reply = send(port, "POST /api/v1/echo HTTP/1.1\r\n" + host +
                                               "Transfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n");
        CHECK(reply.status == 411);
        CHECK(reply.error() == "length_required");
    }
    SECTION("headers over 16 KiB are refused") {
        const HttpReply reply = send(port, "GET /api/v1/ping HTTP/1.1\r\n" + host + "X-Big: " +
                                               QByteArray(17 * 1024, 'a') + "\r\n\r\n");
        CHECK(reply.status == 431);
        CHECK(reply.error() == "headers_too_large");
    }
    SECTION("garbage is a bad request") {
        const HttpReply reply = send(port, "HELLO\r\n\r\n");
        CHECK(reply.status == 400);
        CHECK(reply.error() == "bad_request");
    }
    // the server still serves after each refusal
    CHECK(send(port, get("/api/v1/ping", port, fixture.token)).status == 200);
}

TEST_CASE("Remote API server times out a silent client", "[remote-api][server]") {
    Fixture fixture;
    LC_RemoteApiServer::Limits limits;
    limits.readTimeoutMs = 400; // 15 s in the application
    const quint16 port = fixture.start(limits);

    QElapsedTimer timer;
    timer.start();
    HttpReply reply = send(port, {}); // connects and sends nothing
    CHECK(reply.status == 408);
    CHECK(reply.error() == "request_timeout");
    CHECK(timer.elapsed() < 5000);

    reply = send(port, "GET /api/v1/ping HTTP/1.1\r\nHost: 127.0.0.1"); // stops halfway
    CHECK(reply.status == 408);

    // a silent client does not block the next one for longer than the timeout
    CHECK(send(port, get("/api/v1/ping", port, fixture.token)).status == 200);
}

TEST_CASE("Remote API server: idle and slow clients hold up nobody else", "[remote-api][server]") {
    Fixture fixture;
    const quint16 port = fixture.start(); // the application's limits: 5 s for the headers, 15 s in all

    // four connections that send nothing, and one that stops halfway
    std::vector<std::unique_ptr<QTcpSocket>> idle;
    for (int i = 0; i < 5; ++i) {
        auto socket = std::make_unique<QTcpSocket>();
        socket->connectToHost(QHostAddress::LocalHost, port);
        REQUIRE(socket->waitForConnected(5000));
        idle.push_back(std::move(socket));
    }
    idle.back()->write("GET /api/v1/ping HTTP/1.1\r\nHost: 127.0.0.1");
    idle.back()->waitForBytesWritten(1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    QElapsedTimer timer;
    timer.start();
    HttpReply reply = send(port, get("/api/v1/ping", port, fixture.token));
    CHECK(reply.status == 200);
    CHECK(timer.elapsed() < 2000); // not 5 x 15 s
    reply = send(port, get("/api/v1/gui", port, fixture.token));
    CHECK(reply.status == 200);
    CHECK(timer.elapsed() < 4000);
}

TEST_CASE("Remote API server: the headers must come quickly", "[remote-api][server]") {
    Fixture fixture;
    LC_RemoteApiServer::Limits limits;
    limits.headerTimeoutMs = 300;
    limits.readTimeoutMs = 20000;
    const quint16 port = fixture.start(limits);

    QElapsedTimer timer;
    timer.start();
    HttpReply reply = send(port, {}); // connects and sends nothing
    CHECK(reply.status == 408);
    CHECK(reply.error() == "request_timeout");
    CHECK(timer.elapsed() < 5000);
    // a body may take the full read timeout once the headers are in
    reply = send(port, "POST /api/v1/echo HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\nX-Api-Token: " +
                           fixture.token + "\r\nContent-Length: 2\r\n\r\n{}");
    CHECK(reply.status == 201);
}

namespace {
// A connection that has sent its headers (with the token) and waits for its body.
std::unique_ptr<QTcpSocket> partialRequest(const quint16 port, const QByteArray& token) {
    auto socket = std::make_unique<QTcpSocket>();
    socket->connectToHost(QHostAddress::LocalHost, port);
    REQUIRE(socket->waitForConnected(5000));
    socket->write("POST /api/v1/echo HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\nX-Api-Token: " + token +
                  "\r\nContent-Length: 100\r\n\r\n{");
    socket->waitForBytesWritten(1000);
    return socket;
}
} // namespace

TEST_CASE("Remote API server: connections over the limit are answered 503 at once", "[remote-api][server]") {
    Fixture fixture;
    LC_RemoteApiServer::Limits limits;
    limits.maxConnections = 2;
    const quint16 port = fixture.start(limits);

    // both readers are past their headers: neither gives way
    std::vector<std::unique_ptr<QTcpSocket>> busy;
    busy.push_back(partialRequest(port, fixture.token));
    busy.push_back(partialRequest(port, fixture.token));
    std::this_thread::sleep_for(std::chrono::milliseconds(400)); // both are being read
    QElapsedTimer timer;
    timer.start();
    HttpReply reply = send(port, get("/api/v1/ping", port, fixture.token), 3000);
    CHECK(reply.status == 503);
    CHECK(reply.error() == "server_busy");
    CHECK(timer.elapsed() < 2500);

    // A client that sends its request a while after connecting still reads the
    // 503: the refused connection is read before it closes, not reset.
    for (int i = 0; i < 3; ++i) {
        QTcpSocket late;
        late.connectToHost(QHostAddress::LocalHost, port);
        REQUIRE(late.waitForConnected(5000));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        late.write(get("/api/v1/ping", port, fixture.token));
        late.waitForBytesWritten(1000);
        QByteArray data;
        QElapsedTimer reading;
        reading.start();
        while (reading.elapsed() < 3000 && !data.contains("\r\n\r\n")) {
            if (late.bytesAvailable() > 0 || late.waitForReadyRead(100)) {
                data += late.readAll();
            }
            if (late.state() != QAbstractSocket::ConnectedState) {
                data += late.readAll();
                break;
            }
        }
        CHECK(data.startsWith("HTTP/1.1 503"));
        CHECK(data.contains("server_busy"));
    }

    busy.clear(); // the readers see the connections close and end
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(send(port, get("/api/v1/ping", port, fixture.token)).status == 200);
}

TEST_CASE("Remote API server: idle connections give way to a real client", "[remote-api][server]") {
    Fixture fixture;
    LC_RemoteApiServer::Limits limits;
    limits.maxIncoming = 2;
    const quint16 port = fixture.start(limits); // 5 s for the headers

    // the connections still sending their headers are all ones that send nothing (no token needed for that)
    std::vector<std::unique_ptr<QTcpSocket>> idle;
    for (int i = 0; i < 2; ++i) {
        auto socket = std::make_unique<QTcpSocket>();
        socket->connectToHost(QHostAddress::LocalHost, port);
        REQUIRE(socket->waitForConnected(5000));
        idle.push_back(std::move(socket));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (int round = 0; round < 3; ++round) {
        QElapsedTimer timer;
        timer.start();
        const HttpReply reply = send(port, get("/api/v1/ping", port, fixture.token), 3000);
        CHECK(reply.status == 200);
        CHECK(timer.elapsed() < 1500); // not the 5 s header timeout
        // and the idle ones come back for more
        auto socket = std::make_unique<QTcpSocket>();
        socket->connectToHost(QHostAddress::LocalHost, port);
        REQUIRE(socket->waitForConnected(5000));
        idle.push_back(std::move(socket));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // the oldest idle connection was answered 408 to make room
    QByteArray first;
    QElapsedTimer reading;
    reading.start();
    while (reading.elapsed() < 2000 && !first.contains("\r\n\r\n")) {
        if (idle.front()->bytesAvailable() > 0 || idle.front()->waitForReadyRead(100)) {
            first += idle.front()->readAll();
        }
    }
    CHECK(first.startsWith("HTTP/1.1 408"));
}

TEST_CASE("Remote API server: a burst of whole requests is answered 503, never taken for idle", "[remote-api][server]") {
    Fixture fixture;
    LC_RemoteApiServer::Limits limits;
    limits.maxConnections = 3;
    const quint16 port = fixture.start(limits);

    // two requests wait for their bodies: one reader is free
    std::vector<std::unique_ptr<QTcpSocket>> busy;
    busy.push_back(partialRequest(port, fixture.token));
    busy.push_back(partialRequest(port, fixture.token));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Each client sends its whole request at once, for a GUI route: nothing pumps
    // the GUI thread yet, so the one that gets the free reader keeps it.
    constexpr int Burst = 6;
    std::vector<HttpReply> replies(Burst);
    std::atomic<int> done{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> clients;
    for (int i = 0; i < Burst; ++i) {
        clients.emplace_back([&, i] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            replies[i] = exchange(port, get("/api/v1/gui", port, fixture.token), 10000);
            ++done;
        });
    }
    go = true;
    QElapsedTimer timer;
    timer.start();
    while (done.load() < Burst - 1 && timer.elapsed() < 5000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(done.load() == Burst - 1); // the refusals came at once
    // LibreCAD is free again: the one that was let in runs
    QElapsedTimer pumping;
    pumping.start();
    while (done.load() < Burst && pumping.elapsed() < 10000) {
        pumpFor(20);
    }
    for (std::thread& client : clients) {
        client.join();
    }
    int served = 0;
    int busyAnswers = 0;
    for (const HttpReply& reply : replies) {
        CAPTURE(reply.status, reply.error(), reply.json.value("detail").toString());
        CHECK(reply.status != 408); // their headers had all arrived
        served += reply.status == 200 ? 1 : 0;
        if (reply.status == 503) {
            CHECK(reply.error() == "server_busy");
            CHECK(reply.json.value("hint").toString() == "retry in a moment");
            ++busyAnswers;
        }
    }
    CHECK(served == 1);
    CHECK(busyAnswers == Burst - 1);
}

TEST_CASE("Remote API server: a flood of idle connections does not lock a real client out", "[remote-api][server]") {
    Fixture fixture;
    LC_RemoteApiServer::Limits limits;
    limits.maxIncoming = 16; // filled within a few dozen ms
    const quint16 port = fixture.start(limits);

    // a local program without the token opens connections that send nothing, a few hundred per second
    std::atomic<bool> stop{false};
    std::vector<NativeSocket> flood; // the flooder's own until it is joined
    std::thread flooder([&] {
        while (!stop.load()) {
            for (int i = 0; i < 6; ++i) { // then a sleep of 10 to 16 ms (the timer's resolution)
                const NativeSocket s = connectNative(port);
                if (s != NoSocket) {
                    flood.push_back(s);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int served = 0;
    qint64 slowest = 0;
    for (int i = 0; i < 20; ++i) {
        QElapsedTimer timer;
        timer.start();
        const HttpReply reply = exchange(port, get("/api/v1/ping", port, fixture.token), 5000);
        slowest = std::max(slowest, timer.elapsed());
        CAPTURE(i, reply.status, reply.error());
        CHECK(reply.status == 200);
        served += reply.status == 200 ? 1 : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    stop = true;
    flooder.join();
    CHECK(served == 20);
    CHECK(slowest < 2000);
    CAPTURE(flood.size());
    CHECK(flood.size() > 200); // the flood did run, at a few hundred per second
    // the idle connections that had to make room were answered 408
    QByteArray first = receiveNative(flood.front(), "\r\n\r\n");
    CHECK(first.startsWith("HTTP/1.1 408"));
    for (const NativeSocket s : flood) {
        closeNative(s);
    }
    CHECK(send(port, get("/api/v1/ping", port, fixture.token)).status == 200);
}

TEST_CASE("Remote API server refuses a client without the token before its body", "[remote-api][server]") {
    Fixture fixture;
    const quint16 port = fixture.start(); // 15 s to read a request
    const QByteArray host = "Host: 127.0.0.1:" + QByteArray::number(port) + "\r\n";

    QElapsedTimer timer;
    timer.start();
    // headers with a large Content-Length, and no body yet
    HttpReply reply = send(port, "POST /api/v1/echo HTTP/1.1\r\n" + host + "Content-Length: 1000000\r\n\r\n{", 5000);
    CHECK(reply.status == 401);
    CHECK(reply.error() == "unauthorized");
    CHECK(timer.elapsed() < 2000); // not the 15 s read timeout
    timer.restart();
    reply = send(port, "POST /api/v1/echo HTTP/1.1\r\nHost: evil.com\r\nX-Api-Token: " + fixture.token +
                           "\r\nContent-Length: 1000000\r\n\r\n{", 5000);
    CHECK(reply.status == 421);
    CHECK(timer.elapsed() < 2000);
}

TEST_CASE("Remote API server answers a client that half-closes after its request", "[remote-api][server]") {
    Fixture fixture;
    std::atomic<int> runs{0};
    LC_RouteOptions changes;
    changes.capability = "test";
    changes.changesDocument = true;
    fixture.router.add("POST", "/api/v1/count", changes, [&runs](LC_ApiCall&) {
        ++runs;
        return LC_ApiResponse::ok({{"runs", runs.load()}}, 201);
    });
    const quint16 port = fixture.start();

    Exchange halfClosed;
    halfClosed.halfClose = true;
    HttpReply reply;
    runWhilePumping([&] { reply = exchange(port, get("/api/v1/ping", port, fixture.token), 10000, &halfClosed); });
    CHECK(reply.status == 200);
    CHECK(reply.json.value("pong").toBool());

    runWhilePumping([&] { reply = exchange(port, get("/api/v1/gui", port, fixture.token), 10000, &halfClosed); });
    CHECK(reply.status == 200);
    CHECK(reply.json.value("on_gui_thread").toBool());

    const QByteArray post = "POST /api/v1/count HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\nX-Api-Token: " +
                            fixture.token + "\r\nContent-Length: 0\r\n\r\n";
    runWhilePumping([&] { reply = exchange(port, post, 10000, &halfClosed); });
    CHECK(reply.status == 201);
    CHECK(runs.load() == 1); // it ran once, and its answer arrived

    // HTTP/1.0 gets no interim answer
    Exchange http10;
    http10.halfClose = true;
    runWhilePumping([&] {
        reply = exchange(port, "GET /api/v1/ping HTTP/1.0\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\nX-Api-Token: " +
                                   fixture.token + "\r\n\r\n", 10000, &http10);
    });
    CHECK(reply.status == 200);
    CHECK(http10.interim == 0);
}

TEST_CASE("Remote API server answers a queued request at its client's deadline", "[remote-api][server]") {
    Fixture fixture;
    std::atomic<int> slowRuns{0};
    std::atomic<int> quickRuns{0};
    LC_RouteOptions gui;
    gui.capability = "test";
    gui.timeoutMs = 120000;
    fixture.router.add("POST", "/api/v1/slow", gui, [&slowRuns](LC_ApiCall&) {
        ++slowRuns;
        return LC_ApiResponse::ok({});
    });
    fixture.router.add("POST", "/api/v1/quick", gui, [&quickRuns](LC_ApiCall&) {
        ++quickRuns;
        return LC_ApiResponse::ok({});
    });
    const quint16 port = fixture.start();
    const QByteArray head = "Host: 127.0.0.1:" + QByteArray::number(port) + "\r\nX-Api-Token: " + fixture.token + "\r\n";

    // The first request holds the dispatcher: nothing pumps the GUI thread yet.
    HttpReply first;
    std::thread holder([&] { first = exchange(port, "POST /api/v1/slow HTTP/1.1\r\n" + head + "Content-Length: 0\r\n\r\n", 20000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    QElapsedTimer timer;
    timer.start();
    const HttpReply queued = exchange(port, "POST /api/v1/quick HTTP/1.1\r\n" + head +
                                                "X-Request-Timeout-Ms: 400\r\nContent-Length: 0\r\n\r\n", 10000);
    const qint64 waited = timer.elapsed();
    CHECK(queued.status == 504);
    CHECK(queued.error() == "ui_timeout");
    CHECK_FALSE(queued.json.value("started").toBool(true));
    CHECK(waited < 1500); // at its deadline, while the first one still waits

    pumpFor(300); // LibreCAD is free again: the first one runs, the dropped one never
    holder.join();
    CHECK(first.status == 200);
    CHECK(slowRuns.load() == 1);
    pumpFor(200);
    CHECK(quickRuns.load() == 0);
}

TEST_CASE("Remote API server: a queued request whose client closed never runs", "[remote-api][server]") {
    Fixture fixture;
    std::atomic<int> runs{0};
    LC_RouteOptions gui;
    gui.capability = "test";
    fixture.router.add("POST", "/api/v1/slow", gui, [](LC_ApiCall&) { return LC_ApiResponse::ok({}); });
    fixture.router.add("POST", "/api/v1/count", gui, [&runs](LC_ApiCall&) {
        ++runs;
        return LC_ApiResponse::ok({}, 201);
    });
    const quint16 port = fixture.start();
    const QByteArray head = "Host: 127.0.0.1:" + QByteArray::number(port) + "\r\nX-Api-Token: " + fixture.token + "\r\n";

    HttpReply first;
    std::thread holder([&] { first = exchange(port, "POST /api/v1/slow HTTP/1.1\r\n" + head + "Content-Length: 0\r\n\r\n", 20000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // queued behind it; the client gives up after 300 ms and closes its socket
    const HttpReply abandoned = exchange(port, "POST /api/v1/count HTTP/1.1\r\n" + head + "Content-Length: 0\r\n\r\n", 300);
    CHECK(abandoned.status == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // the reader finds it gone
    pumpFor(300);
    holder.join();
    CHECK(first.status == 200);
    CHECK(runs.load() == 0);
}

TEST_CASE("Remote API server: a client that closes after reading the interim answer is gone", "[remote-api][server]") {
    Fixture fixture;
    std::atomic<int> runs{0};
    LC_RouteOptions gui;
    gui.capability = "test";
    fixture.router.add("POST", "/api/v1/slow", gui, [](LC_ApiCall&) { return LC_ApiResponse::ok({}); });
    fixture.router.add("POST", "/api/v1/count", gui, [&runs](LC_ApiCall&) {
        ++runs;
        return LC_ApiResponse::ok({}, 201);
    });
    LC_RemoteApiServer::Limits limits;
    limits.interimRepeatMs = 200; // 1 s in the application
    const quint16 port = fixture.start(limits);
    const QByteArray head = "Host: 127.0.0.1:" + QByteArray::number(port) + "\r\nX-Api-Token: " + fixture.token + "\r\n";
    const QByteArray count = "POST /api/v1/count HTTP/1.1\r\n" + head + "Content-Length: 0\r\n\r\n";

    SECTION("closed after the first interim answer: the queued request never runs") {
        HttpReply first;
        std::thread holder([&] { first = exchange(port, "POST /api/v1/slow HTTP/1.1\r\n" + head + "Content-Length: 0\r\n\r\n", 20000); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        // as a relay does: the request, a half-close, the interim answer read, then the socket closed
        const NativeSocket s = connectNative(port);
        REQUIRE(s != NoSocket);
        REQUIRE(sendNative(s, count));
        halfCloseNative(s);
        const QByteArray interim = receiveNative(s, "\r\n\r\n");
        CHECK(interim.startsWith("HTTP/1.1 100 Continue\r\n\r\n"));
        closeNative(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(700)); // the next interim answer meets a reset
        pumpFor(300); // LibreCAD is free again
        holder.join();
        CHECK(first.status == 200);
        pumpFor(200);
        CHECK(runs.load() == 0);
    }
    SECTION("still half-closed: several interim answers, then the final one") {
        HttpReply first;
        std::thread holder([&] { first = exchange(port, "POST /api/v1/slow HTTP/1.1\r\n" + head + "Content-Length: 0\r\n\r\n", 20000); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Exchange halfClosed;
        halfClosed.halfClose = true;
        HttpReply reply;
        std::atomic<bool> done{false};
        std::thread client([&] {
            reply = exchange(port, count, 20000, &halfClosed);
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        QElapsedTimer pumping;
        pumping.start();
        while (!done.load() && pumping.elapsed() < 10000) {
            pumpFor(20);
        }
        holder.join();
        client.join();
        CHECK(first.status == 200);
        CHECK(reply.status == 201);
        CHECK(halfClosed.interim >= 2);
        CHECK(runs.load() == 1);
    }
}

TEST_CASE("Remote API server: X-Request-Timeout-Ms is one whole number of milliseconds", "[remote-api][server]") {
    Fixture fixture;
    const quint16 port = fixture.start();
    const QByteArray header = "X-Request-Timeout-Ms: ";

    // A client that believes it capped its wait must not wait without bound: what
    // is not a number of milliseconds from 1 to 600000 is refused, not ignored.
    for (const char* value : {"0", "-1", "abc", "1e3", "700.5", "0x2bc", "+700", "99999999999999999999", "600001", ""}) {
        CAPTURE(value);
        const HttpReply reply = send(port, get("/api/v1/ping", port, fixture.token, header + value + "\r\n"));
        CHECK(reply.status == 400);
        CHECK(reply.error() == "bad_header");
        CHECK(reply.json.value("header").toString() == "X-Request-Timeout-Ms");
        CHECK(reply.json.value("max").toInteger() == 600000);
    }
    // sent twice, the header arrives as "700, 700"
    HttpReply reply = send(port, get("/api/v1/ping", port, fixture.token, header + "700\r\n" + header + "700\r\n"));
    CHECK(reply.status == 400);
    CHECK(reply.error() == "bad_header");
    for (const char* value : {"700", " 700 ", "600000", "0700"}) {
        CAPTURE(value);
        CHECK(send(port, get("/api/v1/ping", port, fixture.token, header + value + "\r\n")).status == 200);
    }
    // 1 ms is a deadline too (the answer may beat it, or be 504)
    reply = send(port, get("/api/v1/ping", port, fixture.token, header + "1\r\n"));
    CHECK((reply.status == 200 || (reply.status == 504 && reply.error() == "ui_timeout")));
    // the token is checked first
    reply = send(port, get("/api/v1/ping", port, {}, header + "abc\r\n"));
    CHECK(reply.status == 401);
}

TEST_CASE("Remote API server answers small requests at once", "[remote-api][server]") {
    Fixture fixture;
    const quint16 port = fixture.start();
    std::vector<qint64> times;
    for (int i = 0; i < 25; ++i) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, port);
        REQUIRE(socket.waitForConnected(5000));
        // from the request to the end of the answer (a server-thread route)
        QElapsedTimer timer;
        timer.start();
        socket.write(get("/api/v1/ping", port, fixture.token));
        socket.flush();
        QByteArray data;
        while (timer.elapsed() < 5000 && !data.contains("\"pong\":true")) {
            if (socket.bytesAvailable() > 0 || socket.waitForReadyRead(5)) {
                data += socket.readAll();
            }
        }
        times.push_back(timer.nsecsElapsed() / 1000);
        REQUIRE(data.startsWith("HTTP/1.1 200"));
    }
    std::sort(times.begin(), times.end());
    // median under 25 ms (the reader that slept 50 ms per answer took 50 to 75)
    CHECK(times.at(times.size() / 2) < 25000);
}

TEST_CASE("Remote API server: a request whose client gave up never runs", "[remote-api][server]") {
    Fixture fixture;
    std::atomic<int> runs{0};
    LC_RouteOptions changes;
    changes.capability = "test";
    changes.changesDocument = true;
    fixture.router.add("POST", "/api/v1/count", changes, [&runs](LC_ApiCall&) {
        ++runs;
        return LC_ApiResponse::ok({{"runs", runs.load()}}, 201);
    });
    const quint16 port = fixture.start();
    const QByteArray request = "POST /api/v1/count HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) +
                               "\r\nX-Api-Token: " + fixture.token + "\r\nContent-Length: 0\r\n\r\n";

    // Nothing pumps the GUI thread (LibreCAD is busy): the client gives up after
    // 300 ms and closes the connection.
    const HttpReply abandoned = exchange(port, request, 300);
    CHECK(abandoned.status == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // the reader sees the close
    pumpFor(300);   // LibreCAD is free again
    CHECK(runs.load() == 0);

    // the next client is served normally
    const HttpReply served = send(port, request);
    CHECK(served.status == 201);
    CHECK(runs.load() == 1);
}

TEST_CASE("Remote API server: the client's deadline caps the wait for LibreCAD", "[remote-api][server]") {
    Fixture fixture;
    std::atomic<int> runs{0};
    LC_RouteOptions slow;
    slow.capability = "test";
    slow.timeoutMs = 120000; // as open, save and export
    fixture.router.add("POST", "/api/v1/slow", slow, [&runs](LC_ApiCall&) {
        ++runs;
        return LC_ApiResponse::ok({});
    });
    const quint16 port = fixture.start();
    QElapsedTimer timer;
    timer.start();
    // nothing pumps the GUI thread
    const HttpReply reply = exchange(port, "POST /api/v1/slow HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) +
                                               "\r\nX-Api-Token: " + fixture.token +
                                               "\r\nX-Request-Timeout-Ms: 400\r\nContent-Length: 0\r\n\r\n",
                                     10000);
    CHECK(reply.status == 504);
    CHECK(reply.error() == "ui_timeout");
    CHECK_FALSE(reply.json.value("started").toBool(true));
    CHECK(timer.elapsed() < 5000);
    pumpFor(200);
    CHECK(runs.load() == 0);
}

TEST_CASE("Remote API server answers Expect: 100-continue before the body", "[remote-api][server]") {
    Fixture fixture;
    const quint16 port = fixture.start();
    const QByteArray body = "{\"a\":1}";
    const QByteArray rest = "Host: 127.0.0.1:" + QByteArray::number(port) + "\r\nX-Api-Token: " + fixture.token +
                            "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";

    SECTION("an HTTP/1.1 client that waits with its body is told to send it at once") {
        // RFC 9110 10.1.1; curl waits a second for it before every body over 1 MiB
        const NativeSocket s = connectNative(port);
        REQUIRE(s != NoSocket);
        QElapsedTimer timer;
        timer.start();
        REQUIRE(sendNative(s, "POST /api/v1/echo HTTP/1.1\r\n" + rest + "Expect: 100-continue\r\n\r\n"));
        REQUIRE(readableWithin(s, 3000));
        CHECK(timer.elapsed() < 1000);
        CHECK(receiveNative(s, "\r\n\r\n") == "HTTP/1.1 100 Continue\r\n\r\n");
        REQUIRE(sendNative(s, body));
        const QByteArray answer = receiveUntilClosed(s);
        CHECK(answer.startsWith("HTTP/1.1 201"));
        CHECK(answer.contains("\"a\":1"));
        closeNative(s);
    }
    SECTION("no interim answer when the body came with the headers") {
        Exchange options;
        const HttpReply reply = exchange(port, "POST /api/v1/echo HTTP/1.1\r\n" + rest + "Expect: 100-continue\r\n\r\n" + body,
                                         10000, &options);
        CHECK(reply.status == 201);
        CHECK(options.interim == 0);
    }
    SECTION("HTTP/1.0 ignores the expectation") {
        const NativeSocket s = connectNative(port);
        REQUIRE(s != NoSocket);
        REQUIRE(sendNative(s, "POST /api/v1/echo HTTP/1.0\r\n" + rest + "Expect: 100-continue\r\n\r\n"));
        CHECK_FALSE(readableWithin(s, 500));
        REQUIRE(sendNative(s, body));
        CHECK(receiveUntilClosed(s).startsWith("HTTP/1.1 201"));
        closeNative(s);
    }
    SECTION("another expectation is refused at once, and so is a client without the token") {
        QElapsedTimer timer;
        timer.start();
        Exchange options;
        HttpReply reply = exchange(port, "POST /api/v1/echo HTTP/1.1\r\n" + rest + "Expect: 200-ok\r\n\r\n", 5000, &options);
        CHECK(reply.status == 417);
        CHECK(reply.error() == "expectation_failed");
        CHECK(reply.json.value("header").toString() == "Expect");
        CHECK(options.interim == 0);
        CHECK(timer.elapsed() < 2000);
        // a repeated header arrives joined: still only 100-continue
        reply = exchange(port, "POST /api/v1/echo HTTP/1.1\r\n" + rest + "Expect: 100-Continue\r\nExpect: 100-continue\r\n\r\n" + body);
        CHECK(reply.status == 201);
        // the final status instead of the 100: the client never sends its body
        options = {};
        reply = exchange(port, "POST /api/v1/echo HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) +
                                   "\r\nContent-Length: 7\r\nExpect: 100-continue\r\n\r\n", 5000, &options);
        CHECK(reply.status == 401);
        CHECK(options.interim == 0);
    }
}

TEST_CASE("Remote API server answers a request with bare LF line endings at once", "[remote-api][server]") {
    Fixture fixture;
    const quint16 port = fixture.start(); // 5 s for the headers
    QElapsedTimer timer;
    timer.start();
    // as typed into ncat: the request never ends with CRLF CRLF
    const HttpReply reply = exchange(port, "GET /api/v1/ping HTTP/1.1\nHost: 127.0.0.1:" + QByteArray::number(port) +
                                               "\nX-Api-Token: " + fixture.token + "\n\n", 8000);
    CHECK(reply.status == 400);
    CHECK(reply.error() == "bad_request");
    CHECK(reply.json.value("detail").toString() == "lines must end with CRLF");
    CHECK(timer.elapsed() < 2000); // not 408 after the 5 s header timeout
    // an empty line before the request line is ignored (RFC 9112 2.2)
    CHECK(exchange(port, "\r\n" + get("/api/v1/ping", port, fixture.token)).status == 200);
}

TEST_CASE("Remote API server: a request dropped while queued leaves the queue with its body", "[remote-api][server]") {
    Fixture fixture;
    std::atomic<int> runs{0};
    LC_RouteOptions gui;
    gui.capability = "test";
    gui.timeoutMs = 120000; // as open, save and export
    fixture.router.add("POST", "/api/v1/slow", gui, [](LC_ApiCall&) { return LC_ApiResponse::ok({}); });
    fixture.router.add("POST", "/api/v1/sink", gui, [&runs](LC_ApiCall&) {
        ++runs;
        return LC_ApiResponse::ok({}, 201);
    });
    const quint16 port = fixture.start();
    const QByteArray head = "Host: 127.0.0.1:" + QByteArray::number(port) + "\r\nX-Api-Token: " + fixture.token + "\r\n";
    const QByteArray body = "{" + QByteArray(256 * 1024, ' ') + "}";
    const QByteArray sink = "POST /api/v1/sink HTTP/1.1\r\n" + head + "Content-Length: " + QByteArray::number(body.size());

    // The first request holds the dispatcher: nothing pumps the GUI thread yet.
    HttpReply first;
    std::thread holder([&] { first = exchange(port, "POST /api/v1/slow HTTP/1.1\r\n" + head + "Content-Length: 0\r\n\r\n", 20000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // answered at their deadline while they wait behind it
    for (int i = 0; i < 5; ++i) {
        CAPTURE(i);
        const HttpReply dropped = exchange(port, sink + "\r\nX-Request-Timeout-Ms: 100\r\n\r\n" + body, 5000);
        CHECK(dropped.status == 504);
    }
    // their bodies went with them, though the dispatcher has not got to them (it would hold them for as
    // long as the first request runs: up to 120 s)
    CHECK(fixture.server->queuedBodyBytes() == 0);

    // one that still waits keeps its body until its client leaves
    const NativeSocket waiting = connectNative(port);
    REQUIRE(waiting != NoSocket);
    REQUIRE(sendNative(waiting, sink + "\r\n\r\n" + body));
    QElapsedTimer timer;
    timer.start();
    while (fixture.server->queuedBodyBytes() != body.size() && timer.elapsed() < 3000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(fixture.server->queuedBodyBytes() == body.size());
    closeNative(waiting);
    timer.restart();
    while (fixture.server->queuedBodyBytes() != 0 && timer.elapsed() < 3000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(fixture.server->queuedBodyBytes() == 0);

    pumpFor(300); // LibreCAD is free again: the first one runs, the dropped ones never
    holder.join();
    CHECK(first.status == 200);
    pumpFor(200);
    CHECK(runs.load() == 0);
}

TEST_CASE("Remote API server: a second server cannot take the port", "[remote-api][server]") {
    Fixture fixture;
    const quint16 port = fixture.start();

    LC_RemoteApiServer second(fixture.router, fixture.context);
    QString error;
    CHECK(second.start(port, QString::fromUtf8(fixture.token), &error) == LC_RemoteApiServer::StartResult::PortInUse);
    CHECK_FALSE(error.isEmpty());
    CHECK_FALSE(second.isListening());
    CHECK_FALSE(second.isRunning());

    // the first one is unaffected
    CHECK(send(port, get("/api/v1/ping", port, fixture.token)).status == 200);

    LC_RemoteApiServer noToken(fixture.router, fixture.context);
    CHECK(noToken.start(0, QString(), &error) == LC_RemoteApiServer::StartResult::Failed); // fail closed
}

TEST_CASE("Remote API server stops quickly", "[remote-api][server]") {
    Fixture fixture;

    SECTION("with a silent client connected") {
        const quint16 port = fixture.start();
        HttpReply reply;
        std::thread client([&] { reply = exchange(port, {}, 5000); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        QElapsedTimer timer;
        timer.start();
        fixture.server->stop();
        CHECK(timer.elapsed() < 1000);
        client.join();
        CHECK(reply.status == 0); // dropped without an answer
        CHECK_FALSE(fixture.server->isListening());
    }
    SECTION("while a request waits for the GUI thread") {
        const quint16 port = fixture.start();
        HttpReply reply;
        // nothing pumps the GUI thread, so the GUI route cannot run
        std::thread client([&] { reply = exchange(port, get("/api/v1/gui", port, fixture.token), 5000); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        QElapsedTimer timer;
        timer.start();
        fixture.server->stop();
        CHECK(timer.elapsed() < 1000);
        client.join();
        CHECK(reply.status == 503);
        CHECK(reply.error() == "shutting_down");
        QCoreApplication::processEvents(); // the abandoned task does not run
    }
    // stop() is idempotent, and a stopped port is free again
    fixture.server->stop();
    QTcpServer probe;
    CHECK(probe.listen(QHostAddress::LocalHost, fixture.server->port()));
}

TEST_CASE("Remote API controller serves /status and writes the discovery file", "[remote-api][server][controller]") {
    (void)lc::test::application();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QByteArray savedDir = qgetenv("LIBRECAD_SETTINGS_DIR");
    const bool dirWasSet = qEnvironmentVariableIsSet("LIBRECAD_SETTINGS_DIR");
    qputenv("LIBRECAD_SETTINGS_DIR", dir.path().toLocal8Bit());
    const QString discoveryPath = dir.filePath("remote-api.json");

    const quint16 port = freePort();
    LC_RemoteApiConfig config;
    config.enabled = true;
    config.port = port;
    config.token.clear(); // generated on the first start
    config.save();

    {
        LC_RemoteApiController controller(nullptr);
        CHECK(controller.statusText() == "Disabled");
        controller.startIfEnabled();
        REQUIRE(controller.state() == LC_RemoteApiController::State::Listening);
        CHECK(controller.port() == port);
        CHECK(controller.statusText() == QString("Listening on 127.0.0.1:%1").arg(port));

        const QString token = LC_RemoteApiConfig::load().token;
        CHECK(token.size() == 32); // generated and saved

        CHECK(QDir::cleanPath(controller.discoveryFile()) == QDir::cleanPath(discoveryPath));
        const QJsonObject discovery = LC_RemoteApiConfig::readDiscoveryFile(discoveryPath);
        CHECK(discovery.value("url").toString() == QString("http://127.0.0.1:%1").arg(port));
        CHECK(discovery.value("port").toInt() == port);
        CHECK(discovery.value("token").toString() == token);
        CHECK(static_cast<qint64>(discovery.value("pid").toDouble()) == QCoreApplication::applicationPid());
        CHECK(discovery.value("app").toString() == "LibreCAD");
        CHECK(discovery.value("api_version").toString() == "1.0");
        CHECK(discovery.value("mcp_build").toInt() == LC_RemoteApiController::mcpBuild());
        CHECK(LC_RemoteApiController::mcpBuild() >= 1);
        CHECK(discovery.value("app_version").toString() == LC_RemoteApiController::appVersion());
        CHECK(discovery.value("started_at").toString().endsWith("Z"));

        HttpReply reply = send(port, get("/api/v1/status", port, token.toUtf8()));
        REQUIRE(reply.status == 200);
        CHECK(reply.json.value("app").toString() == "LibreCAD");
        CHECK(reply.json.value("api_version").toString() == "1.0");
        CHECK(reply.json.value("mcp_build").toInt() == LC_RemoteApiController::mcpBuild());
        const QJsonArray capabilities = reply.json.value("capabilities").toArray();
        CHECK(capabilities.contains("status"));
        CHECK(capabilities.contains("routes"));
        CHECK(QDir::cleanPath(reply.json.value("discovery_file").toString()) == QDir::cleanPath(discoveryPath));
        CHECK(reply.json.value("documents").toArray().isEmpty());
        CHECK(reply.json.value("active_document").isNull());
        CHECK(reply.json.value("commands_allowed").toBool());
        CHECK(reply.json.value("gui").toObject().value("modal_open").toBool() == false);
        CHECK_FALSE(reply.body.contains(token.toUtf8())); // /status never shows the token

        CHECK(send(port, get("/api/v1/status", port, {})).status == 401);

        reply = send(port, get("/api/v1/routes", port, token.toUtf8()));
        REQUIRE(reply.status == 200);
        QStringList paths;
        for (const QJsonValue& route : reply.json.value("routes").toArray()) {
            paths << route.toObject().value("path").toString();
        }
        CHECK(paths.contains("/api/v1/status"));
        CHECK(paths.contains("/api/v1/routes"));

        // policy changes need no restart
        LC_RemoteApiConfig changed = LC_RemoteApiConfig::load();
        changed.allowCommands = false;
        changed.save();
        controller.applySettings();
        CHECK(controller.port() == port);
        reply = send(port, get("/api/v1/status", port, token.toUtf8()));
        CHECK(reply.json.value("commands_allowed").toBool() == false);

        // a new token restarts the server
        changed.token = LC_RemoteApiConfig::generateToken();
        changed.save();
        controller.applySettings();
        REQUIRE(controller.isListening());
        CHECK(send(port, get("/api/v1/status", port, token.toUtf8())).status == 401);
        CHECK(send(port, get("/api/v1/status", port, changed.token.toUtf8())).status == 200);
        CHECK(LC_RemoteApiConfig::readDiscoveryFile(discoveryPath).value("token").toString() == changed.token);

        // the port is taken by someone else
        controller.stop();
        CHECK_FALSE(QFile::exists(discoveryPath));
        {
            QTcpServer blocker;
            REQUIRE(blocker.listen(QHostAddress::LocalHost, port));
            controller.startIfEnabled();
            CHECK(controller.state() == LC_RemoteApiController::State::PortInUse);
            CHECK(controller.statusText() == QString("Port %1 is in use").arg(port));
            CHECK_FALSE(QFile::exists(discoveryPath));
        }
        controller.applySettings(); // retried on OK in Preferences
        CHECK(controller.state() == LC_RemoteApiController::State::Listening);

        // disabled in Preferences
        changed.enabled = false;
        changed.save();
        controller.applySettings();
        CHECK(controller.state() == LC_RemoteApiController::State::Disabled);
        CHECK(controller.statusText() == "Disabled");
        CHECK_FALSE(QFile::exists(discoveryPath));
        CHECK(send(port, get("/api/v1/status", port, changed.token.toUtf8())).status == 0); // nobody listens

        changed.enabled = true;
        changed.save();
        controller.startIfEnabled();
        CHECK(QFile::exists(discoveryPath));
    }
    // the destructor stops the server and removes the file
    CHECK_FALSE(QFile::exists(discoveryPath));

    LC_RemoteApiConfig defaults;
    defaults.save();
    if (dirWasSet) {
        qputenv("LIBRECAD_SETTINGS_DIR", savedDir);
    }
    else {
        qunsetenv("LIBRECAD_SETTINGS_DIR");
    }
}
