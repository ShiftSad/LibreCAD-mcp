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

// The Remote API over real loopback sockets (design section 5.1, "Server"):
// authentication, Host and Origin checks, limits, GUI routes, binding, stop(),
// and the controller with its discovery file and /status, without a main window.

#include <atomic>
#include <chrono>
#include <thread>

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

// One exchange on a fresh connection: send request (may be empty: a silent
// client), then read until the server closes or timeoutMs passes. Runs on any thread.
HttpReply exchange(const quint16 port, const QByteArray& request, const int timeoutMs = 10000) {
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
