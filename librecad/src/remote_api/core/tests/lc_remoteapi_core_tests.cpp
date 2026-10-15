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

// Core of the Remote API without a main window (design section 5.1, "Core"):
// HTTP parser and serializer, router, strict JSON reader, settings, token,
// discovery file, GUI runner and modal guard.

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include "lc_actiontestsupport.h"
#include "lc_remoteapiconfig.h"
#include "lc_remoteapicontext.h"
#include "lc_remoteapicontroller.h"
#include "lc_remoteapigui.h"
#include "lc_remoteapihttp.h"
#include "lc_remoteapijson.h"
#include "lc_remoteapimodalguard.h"
#include "lc_remoteapioptionswidget.h"
#include "lc_remoteapirouter.h"
#include "rs_settings.h"

namespace {

using State = LC_HttpRequestParser::State;

LC_HttpRequestParser parse(const QByteArray& bytes) {
    LC_HttpRequestParser parser;
    parser.feed(bytes);
    return parser;
}

// Runs work on a std::thread while this (the GUI) thread processes events, so
// that tasks it posts to the GUI thread can run. Fails the test after 20 s.
void runWhilePumping(const std::function<void()>& work) {
    std::atomic<bool> done{false};
    std::thread worker([&] {
        work();
        done = true;
    });
    QElapsedTimer timer;
    timer.start();
    while (!done.load() && timer.elapsed() < 20000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    worker.join();
    REQUIRE(done.load());
}

void pumpFor(const int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Rejects every visible dialog after ms, so that a guard that fails to close one
// makes the test fail instead of hanging in exec(). Disarmed when it goes.
class DialogWatchdog {
public:
    explicit DialogWatchdog(const int ms) {
        m_timer.setSingleShot(true);
        QObject::connect(&m_timer, &QTimer::timeout, [this] {
            m_fired = true;
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (auto* dialog = qobject_cast<QDialog*>(widget); dialog != nullptr && dialog->isVisible()) {
                    dialog->reject();
                }
            }
        });
        m_timer.start(ms);
    }
    bool fired() const { return m_fired; }

private:
    QTimer m_timer;
    bool m_fired = false;
};

LC_ApiRequest makeRequest(const QByteArray& method, const QString& path, const QByteArray& body = {}) {
    LC_ApiRequest request;
    request.method = method;
    request.path = path;
    request.body = body;
    return request;
}

QJsonObject bodyOf(const LC_ApiResponse& response) {
    return response.json;
}

} // namespace

// ---------------------------------------------------------------------------
// HTTP parser

TEST_CASE("Remote API parser decodes a GET with query", "[remote-api][core][http]") {
    const auto parser = parse(
        "GET /api/v1/entities?type=line&type=arc&layer=a%2Cb&name=hello+world%21&flag HTTP/1.1\r\n"
        "Host: 127.0.0.1:13140\r\n"
        "X-API-Token: abc\r\n"
        "\r\n");
    REQUIRE(parser.state() == State::Complete);
    const LC_ApiRequest& request = parser.request();
    CHECK(request.method == "GET");
    CHECK(request.path == "/api/v1/entities");
    CHECK(request.rawPath == "/api/v1/entities");
    CHECK(request.query.values("type") == QStringList{"line", "arc"}); // request order
    CHECK(request.query.value("layer") == "a,b");
    CHECK(request.query.value("name") == "hello world!");
    CHECK(request.query.contains("flag"));
    CHECK(request.query.value("flag").isEmpty());
    CHECK(request.headers.value("x-api-token") == "abc"); // names are lower-cased
    CHECK(request.headers.value("host") == "127.0.0.1:13140");
    CHECK(request.body.isEmpty());
}

TEST_CASE("Remote API parser keeps an encoded slash in the raw path", "[remote-api][core][http]") {
    const auto parser = parse("PATCH /api/v1/layers/walls%2Fnew%20one HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(parser.state() == State::Complete);
    CHECK(parser.request().rawPath == "/api/v1/layers/walls%2Fnew%20one");
    CHECK(parser.request().path == "/api/v1/layers/walls/new one");
}

TEST_CASE("Remote API parser assembles a body fed in three parts", "[remote-api][core][http]") {
    LC_HttpRequestParser parser;
    const QByteArray body = R"({"entities":[{"type":"line"}]})";
    const QByteArray head = "POST /api/v1/entities HTTP/1.1\r\nHost: localhost:13140\r\nContent-Type: application/json\r\n"
                            "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
    const QByteArray all = head + body;
    CHECK(parser.feed(all.left(20)) == State::Incomplete);
    CHECK(parser.feed(all.mid(20, head.size())) == State::Incomplete);
    CHECK(parser.feed(all.mid(20 + head.size())) == State::Complete);
    CHECK(parser.request().method == "POST");
    CHECK(parser.request().body == body);
    CHECK(parser.request().headers.value("content-type") == "application/json");
}

TEST_CASE("Remote API parser enforces its limits", "[remote-api][core][http]") {
    SECTION("headers over 16 KiB give 431") {
        QByteArray request = "GET /api/v1/status HTTP/1.1\r\nHost: x\r\nX-Big: ";
        request += QByteArray(LC_HttpRequestParser::DefaultMaxHeaderBytes, 'a');
        request += "\r\n\r\n";
        const auto parser = parse(request);
        REQUIRE(parser.state() == State::Error);
        CHECK(parser.error().status == 431);
        CHECK(parser.error().token == "headers_too_large");
    }
    SECTION("headers without an end over 16 KiB give 431 before the end arrives") {
        LC_HttpRequestParser parser;
        parser.feed("GET / HTTP/1.1\r\nX-Big: ");
        CHECK(parser.feed(QByteArray(LC_HttpRequestParser::DefaultMaxHeaderBytes, 'a')) == State::Error);
        CHECK(parser.error().status == 431);
    }
    SECTION("a body over 4 MiB gives 413 from Content-Length alone") {
        const auto parser = parse("POST /api/v1/entities HTTP/1.1\r\nHost: x\r\nContent-Length: " +
                                  QByteArray::number(LC_HttpRequestParser::DefaultMaxBodyBytes + 1) + "\r\n\r\n{");
        REQUIRE(parser.state() == State::Error);
        CHECK(parser.error().status == 413);
        CHECK(parser.error().token == "body_too_large");
        CHECK(parser.error().extra.value("limit").toInteger() == LC_HttpRequestParser::DefaultMaxBodyBytes);
    }
    SECTION("exactly 4 MiB is accepted") {
        LC_HttpRequestParser parser;
        const QByteArray head = QByteArray("POST /x HTTP/1.1\r\nContent-Length: ") +
                                QByteArray::number(LC_HttpRequestParser::DefaultMaxBodyBytes) + QByteArray("\r\n\r\n");
        parser.feed(head);
        CHECK(parser.feed(QByteArray(LC_HttpRequestParser::DefaultMaxBodyBytes, ' ')) == State::Complete);
    }
    SECTION("chunked transfer encoding gives 411") {
        const auto parser = parse("POST /api/v1/entities HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
        REQUIRE(parser.state() == State::Error);
        CHECK(parser.error().status == 411);
        CHECK(parser.error().token == "length_required");
    }
}

TEST_CASE("Remote API parser refuses malformed requests", "[remote-api][core][http]") {
    for (const QByteArray& bad : {QByteArray("garbage\r\n\r\n"), QByteArray("GET\r\n\r\n"),
                                  QByteArray("get /x HTTP/1.1\r\n\r\n"), QByteArray("GET x HTTP/1.1\r\n\r\n"),
                                  QByteArray("GET /x HTTP/2\r\n\r\n"), QByteArray("GET http://h/x HTTP/1.1\r\n\r\n"),
                                  QByteArray("GET /x HTTP/1.1\r\nNoColon\r\n\r\n"),
                                  QByteArray("GET /x HTTP/1.1\r\nContent-Length: 12abc\r\n\r\n"),
                                  QByteArray("GET /x HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n"),
                                  QByteArray("GET /x HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n"),
                                  QByteArray("GET /x HTTP/1.1\r\nA: b\r\n  folded\r\n\r\n")}) {
        CAPTURE(bad);
        const auto parser = parse(bad);
        REQUIRE(parser.state() == State::Error);
        CHECK(parser.error().status == 400);
        CHECK(parser.error().token == "bad_request");
    }
}

TEST_CASE("Remote API parser reports a short body at the end of the connection", "[remote-api][core][http]") {
    LC_HttpRequestParser parser;
    CHECK(parser.feed("POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\n12345") == State::Incomplete);
    CHECK(parser.finish() == State::Error);
    CHECK(parser.error().status == 400);

    LC_HttpRequestParser empty;
    CHECK(empty.isEmpty());
    LC_HttpRequestParser partialHead;
    partialHead.feed("GET /x HTT");
    CHECK_FALSE(partialHead.isEmpty());
    CHECK(partialHead.finish() == State::Error);
}

// ---------------------------------------------------------------------------
// Serialization

TEST_CASE("Remote API serializes JSON and raw responses", "[remote-api][core][http]") {
    SECTION("JSON with the common headers") {
        LC_ApiResponse response = LC_ApiError(404, "not_found", "no route").toResponse();
        response.extraHeaders.append(qMakePair(QByteArray("X-Test"), QByteArray("a\r\nInjected: yes")));
        const QByteArray out = LC_RemoteApiHttp::serializeResponse(response);
        const qsizetype split = out.indexOf("\r\n\r\n");
        REQUIRE(split > 0);
        const QByteArray head = out.left(split);
        const QByteArray body = out.mid(split + 4);
        CHECK(head.startsWith("HTTP/1.1 404 Not Found\r\n"));
        CHECK(head.contains("\r\nContent-Type: application/json; charset=utf-8"));
        const QByteArray lengthHeader = QByteArray("\r\nContent-Length: ") + QByteArray::number(body.size());
        CHECK(head.contains(lengthHeader));
        CHECK(head.contains("\r\nCache-Control: no-store"));
        CHECK(head.contains("\r\nX-Content-Type-Options: nosniff"));
        CHECK(head.contains("\r\nConnection: close"));
        CHECK_FALSE(head.contains("\r\nInjected:")); // CR/LF in a header value cannot add a header
        CHECK(body == R"({"detail":"no route","error":"not_found"})");
    }
    SECTION("raw bytes keep their content type") {
        const QByteArray png("\x89PNG\r\n\x1a\n\0\0", 10);
        LC_ApiResponse response = LC_ApiResponse::bytes(png, "image/png");
        response.extraHeaders.append(qMakePair(QByteArray("X-LibreCAD-Window"), QByteArray("0,0,10,5")));
        const QByteArray out = LC_RemoteApiHttp::serializeResponse(response);
        CHECK(out.startsWith("HTTP/1.1 200 OK\r\n"));
        CHECK(out.contains("\r\nContent-Type: image/png\r\n"));
        CHECK(out.contains("\r\nContent-Length: 10\r\n"));
        CHECK(out.contains("\r\nX-LibreCAD-Window: 0,0,10,5\r\n"));
        CHECK(out.endsWith(png));
    }
    SECTION("reason phrases of the catalog") {
        CHECK(LC_RemoteApiHttp::reasonPhrase(201) == "Created");
        CHECK(LC_RemoteApiHttp::reasonPhrase(421) == "Misdirected Request");
        CHECK(LC_RemoteApiHttp::reasonPhrase(504) == "Gateway Timeout");
    }
}

TEST_CASE("Remote API error envelope", "[remote-api][core]") {
    const LC_ApiError error(409, "file_exists", "already there", {{"path", "C:/a.dxf"}, {"error", "spoofed"}});
    const LC_ApiResponse response = error.toResponse();
    CHECK(response.status == 409);
    CHECK(response.json.value("error").toString() == "file_exists"); // extra cannot override
    CHECK(response.json.value("detail").toString() == "already there");
    CHECK(response.json.value("path").toString() == "C:/a.dxf");
    CHECK(QByteArray(error.what()) == "file_exists: already there");
    CHECK_FALSE(LC_ApiError(401, "unauthorized").toResponse().json.contains("detail"));
}

// ---------------------------------------------------------------------------
// Router

TEST_CASE("Remote API router matches methods and paths", "[remote-api][core][router]") {
    (void)lc::test::application();
    LC_RemoteApiContext context(nullptr);
    LC_RemoteApiRouter router;
    LC_RouteOptions options;
    options.capability = "items";
    options.summary = "test";
    options.gui = false;
    router.add("GET", "/api/v1/status", options, [](LC_ApiCall&) { return LC_ApiResponse::ok({{"route", "status"}}); });
    router.add("GET", "/api/v1/items/{id}", options, [](LC_ApiCall& call) {
        return LC_ApiResponse::ok({{"route", "get"}, {"id", call.params.value("id")}});
    });
    router.add("PATCH", "/api/v1/items/{id}", options, [](LC_ApiCall& call) {
        return LC_ApiResponse::ok({{"route", "patch"}, {"id", call.params.value("id")}});
    });
    router.add("POST", "/api/v1/items/{id}", options, [](LC_ApiCall&) { return LC_ApiResponse::ok({{"route", "post-id"}}); });
    router.add("POST", "/api/v1/items/delete", options, [](LC_ApiCall&) { return LC_ApiResponse::ok({{"route", "delete"}}); });
    router.add("GET", "/api/v1/layers/{name}/current", options, [](LC_ApiCall& call) {
        return LC_ApiResponse::ok({{"name", call.params.value("name")}});
    });

    SECTION("a {param} captures one percent-decoded segment") {
        LC_ApiRequest request = makeRequest("GET", "/api/v1/layers/a/b c/current");
        request.rawPath = "/api/v1/layers/a%2Fb%20c/current";
        const LC_ApiResponse response = router.dispatch(request, context);
        CHECK(response.status == 200);
        CHECK(bodyOf(response).value("name").toString() == "a/b c");

        const LC_ApiResponse byId = router.dispatch(makeRequest("PATCH", "/api/v1/items/42"), context);
        CHECK(bodyOf(byId).value("route").toString() == "patch");
        CHECK(bodyOf(byId).value("id").toString() == "42");
    }
    SECTION("a literal segment wins over a {param}") {
        CHECK(bodyOf(router.dispatch(makeRequest("POST", "/api/v1/items/delete"), context)).value("route").toString() == "delete");
        CHECK(bodyOf(router.dispatch(makeRequest("POST", "/api/v1/items/7"), context)).value("route").toString() == "post-id");
    }
    SECTION("an unknown path is a bare 404 not_found") {
        const LC_ApiResponse response = router.dispatch(makeRequest("GET", "/api/v1/nope"), context);
        CHECK(response.status == 404);
        CHECK(response.json.value("error").toString() == "not_found");
    }
    SECTION("matching is exact") {
        CHECK(router.dispatch(makeRequest("GET", "/api/v1/statuses"), context).status == 404);
        CHECK(router.dispatch(makeRequest("GET", "/api/v1/status/"), context).status == 404);
        CHECK(router.dispatch(makeRequest("GET", "/api/v1//status"), context).status == 404);
        CHECK(router.dispatch(makeRequest("GET", "/api/v1/items/"), context).status == 404);
        CHECK(router.dispatch(makeRequest("GET", "/API/v1/status"), context).status == 404);
        CHECK(router.dispatch(makeRequest("GET", "/api/v1/status"), context).status == 200);
    }
    SECTION("a known path with another method is 405 with the allowed methods") {
        const LC_ApiResponse response = router.dispatch(makeRequest("DELETE", "/api/v1/items/3"), context);
        CHECK(response.status == 405);
        CHECK(response.json.value("error").toString() == "method_not_allowed");
        CHECK(response.json.value("allowed").toArray() == QJsonArray{"GET", "PATCH", "POST"});
        REQUIRE(response.extraHeaders.size() == 1);
        CHECK(response.extraHeaders.first().first == "Allow");
        CHECK(response.extraHeaders.first().second == "GET, PATCH, POST");
        CHECK(router.dispatch(makeRequest("OPTIONS", "/api/v1/status"), context).status == 405);
    }
    SECTION("describe and capabilities list every route") {
        const QJsonArray routes = router.describe();
        REQUIRE(routes.size() == 6);
        const QJsonObject first = routes.at(0).toObject();
        CHECK(first.value("method").toString() == "GET");
        CHECK(first.value("path").toString() == "/api/v1/status");
        CHECK(first.value("capability").toString() == "items");
        CHECK(first.value("changes_document").toBool() == false);
        CHECK(first.value("timeout_s").toDouble() == 10.0);
        CHECK(first.value("summary").toString() == "test");
        CHECK(router.capabilities() == QStringList{"items"});
    }
}

TEST_CASE("Remote API router turns failures into envelopes", "[remote-api][core][router]") {
    (void)lc::test::application();
    LC_RemoteApiContext context(nullptr);
    LC_RemoteApiRouter router;
    LC_RouteOptions gui;            // runs inline: the test thread is the GUI thread
    gui.capability = "test";
    LC_RouteOptions direct = gui;
    direct.gui = false;
    router.add("GET", "/api/v1/api-error", gui, [](LC_ApiCall&) -> LC_ApiResponse {
        throw LC_ApiError(404, "unknown_entity", "no such entity", {{"ids", QJsonArray{5, 6}}});
    });
    router.add("GET", "/api/v1/std-error", gui, [](LC_ApiCall&) -> LC_ApiResponse {
        throw std::runtime_error("boom");
    });
    router.add("GET", "/api/v1/std-error-direct", direct, [](LC_ApiCall&) -> LC_ApiResponse {
        throw std::runtime_error("bang");
    });
    router.add("POST", "/api/v1/body", gui, [](LC_ApiCall& call) {
        const LC_JsonReader& body = call.body();
        body.allowOnly({"name"});
        call.warnings.append("careful");
        call.cancelledAction = "Line";
        return LC_ApiResponse::ok({{"name", body.str("name", "none")}}, 201);
    });
    LC_RouteOptions needsDoc = gui;
    needsDoc.needsDocument = true;
    router.add("GET", "/api/v1/needs-doc", needsDoc, [](LC_ApiCall&) { return LC_ApiResponse::ok({}); });

    const LC_ApiResponse apiError = router.dispatch(makeRequest("GET", "/api/v1/api-error"), context);
    CHECK(apiError.status == 404);
    CHECK(apiError.json.value("error").toString() == "unknown_entity");
    CHECK(apiError.json.value("ids").toArray() == QJsonArray{5, 6});

    const LC_ApiResponse stdError = router.dispatch(makeRequest("GET", "/api/v1/std-error"), context);
    CHECK(stdError.status == 500);
    CHECK(stdError.json.value("error").toString() == "internal");
    CHECK(stdError.json.value("detail").toString() == "boom");
    CHECK(router.dispatch(makeRequest("GET", "/api/v1/std-error-direct"), context).status == 500);

    SECTION("body: empty reads as {}, warnings and cancelled_action are added") {
        const LC_ApiResponse response = router.dispatch(makeRequest("POST", "/api/v1/body"), context);
        CHECK(response.status == 201);
        CHECK(response.json.value("name").toString() == "none");
        CHECK(response.json.value("warnings").toArray() == QJsonArray{"careful"});
        CHECK(response.json.value("cancelled_action").toString() == "Line");
    }
    SECTION("body errors") {
        CHECK(router.dispatch(makeRequest("POST", "/api/v1/body", "{nope"), context).json.value("error").toString() == "invalid_json");
        CHECK(router.dispatch(makeRequest("POST", "/api/v1/body", "[1,2]"), context).json.value("error").toString() ==
              "body_must_be_object");
        const LC_ApiResponse unknown = router.dispatch(makeRequest("POST", "/api/v1/body", R"({"bogus":1})"), context);
        CHECK(unknown.status == 400);
        CHECK(unknown.json.value("error").toString() == "unknown_field");
        CHECK(unknown.json.value("field").toString() == "bogus");
    }
    SECTION("needsDocument without a document is 409 no_document") {
        const LC_ApiResponse response = router.dispatch(makeRequest("GET", "/api/v1/needs-doc"), context);
        CHECK(response.status == 409);
        CHECK(response.json.value("error").toString() == "no_document");
    }
}

// ---------------------------------------------------------------------------
// LC_JsonReader

TEST_CASE("Remote API JSON reader is strict and names the path", "[remote-api][core][json]") {
    const QJsonObject body = QJsonDocument::fromJson(R"({
        "layer": "walls", "select": true, "count": 3, "ratio": 0.5, "nothing": null,
        "center": [1, 2], "p3": [1, 2, 3], "p4": [1, 2, 3, 4], "bad": ["x", 1],
        "mode": "window", "spec": {"type": "circle", "radius": "big"},
        "entities": [{"type": "line"}, {"type": "arc"}, {"type": "circle", "radiuss": 2}],
        "vertices": [[0, 0], [1, 0], [1, 1]]
    })").object();
    const LC_JsonReader reader(body);

    auto errorOf = [](const std::function<void()>& call) -> LC_ApiError {
        try {
            call();
        }
        catch (const LC_ApiError& e) {
            return e;
        }
        FAIL("no LC_ApiError thrown");
        return LC_ApiError(0, {});
    };

    CHECK(reader.str("layer") == "walls");
    CHECK(reader.flag("select", false));
    CHECK(reader.integer("count", 0, 0, 10) == 3);
    CHECK(reader.num("ratio") == 0.5);
    CHECK(reader.num("missing", 7.0) == 7.0);
    CHECK_FALSE(reader.has("nothing")); // null reads as absent
    CHECK(reader.str("nothing", "default") == "default");
    CHECK(reader.point("center") == RS_Vector(1, 2));
    CHECK(reader.point("p3") == RS_Vector(1, 2)); // a third value is ignored
    CHECK_FALSE(reader.optPoint("missing").has_value());
    CHECK(reader.points("vertices", 2).size() == 3);
    CHECK(reader.enumOf("mode", {"crossing", "window"}, "crossing") == "window");
    CHECK(reader.object("spec").str("type") == "circle");
    CHECK(reader.objects("entities").size() == 3);

    LC_ApiError e = errorOf([&] { (void)reader.num("missing"); });
    CHECK(e.status == 400);
    CHECK(e.token == "missing_param");
    CHECK(e.extra.value("param").toString() == "missing");

    e = errorOf([&] { (void)reader.object("spec").num("radius"); });
    CHECK(e.token == "bad_param");
    CHECK(e.extra.value("param").toString() == "spec.radius");

    e = errorOf([&] { reader.objects("entities").at(2).allowOnly({"type", "center", "radius"}); });
    CHECK(e.token == "unknown_field");
    CHECK(e.extra.value("field").toString() == "entities[2].radiuss");

    e = errorOf([&] { (void)reader.enumOf("mode", {"crossing", "touch"}, "crossing"); });
    CHECK(e.token == "bad_param");
    CHECK(e.extra.value("param").toString() == "mode");
    CHECK(e.extra.value("allowed").toArray() == QJsonArray{"crossing", "touch"});

    CHECK(errorOf([&] { (void)reader.point("p4"); }).extra.value("param").toString() == "p4");
    CHECK(errorOf([&] { (void)reader.point("bad"); }).token == "bad_param");
    CHECK(errorOf([&] { (void)reader.points("vertices", 4); }).token == "bad_param");
    CHECK(errorOf([&] { (void)reader.integer("count", 0, 5, 10); }).token == "bad_param");
    CHECK(errorOf([&] { (void)reader.integer("ratio", 0, 0, 10); }).token == "bad_param"); // not integral
    CHECK(errorOf([&] { (void)reader.flag("layer", false); }).token == "bad_param");
    CHECK(errorOf([&] { (void)reader.str("count"); }).token == "bad_param");
    CHECK(errorOf([&] { (void)reader.objects("layer"); }).token == "bad_param");
    CHECK(errorOf([&] { reader.allowOnly({"layer"}); }).token == "unknown_field");
}

TEST_CASE("Remote API JSON writers and entity type names", "[remote-api][core][json]") {
    CHECK(lcJsonPoint(RS_Vector(1.5, -2)).toArray() == QJsonArray{1.5, -2});
    CHECK(lcJsonPoint(RS_Vector(false)).isNull());
    CHECK(lcJsonPoint(RS_Vector(std::nan(""), 1)).isNull());

    CHECK(lcEntityTypeName(RS2::EntityLine) == "line");
    CHECK(lcEntityTypeName(RS2::EntitySplinePoints) == "spline");
    CHECK(lcEntityTypeName(RS2::EntityDimAligned) == "dimension");
    CHECK(lcEntityTypeName(RS2::EntityDimLeader) == "leader");
    CHECK(lcEntityTypeName(RS2::EntityMLeader) == "mleader");
    CHECK(lcEntityTypeName(RS2::EntityConstructionLine) == "other");
    for (const char* name : {"line", "circle", "arc", "ellipse", "point", "polyline", "spline", "text", "mtext",
                             "hatch", "insert", "dimension", "image", "leader", "mleader", "solid", "parabola",
                             "hyperbola"}) {
        CAPTURE(name);
        const RS2::EntityType type = lcEntityTypeFromName(name);
        CHECK(type != RS2::EntityUnknown);
        CHECK(lcEntityTypeName(type) == name);
    }
    CHECK(lcEntityTypeFromName("bogus") == RS2::EntityUnknown);
    CHECK(lcEntityTypeFromName("other") == RS2::EntityUnknown);
}

// ---------------------------------------------------------------------------
// Settings, token, discovery file

TEST_CASE("Remote API settings round-trip through RS_Settings", "[remote-api][core][config]") {
    (void)lc::test::application();
    LC_RemoteApiConfig config;
    config.enabled = true;
    config.port = 14141;
    config.token = "0123456789abcdef0123456789abcdef";
    config.notify = false;
    config.allowCommands = false;
    config.allowedRoots = {"C:/work", "D:/cad drawings"};
    config.allowNetworkPaths = true;
    config.save();

    LC_RemoteApiConfig loaded = LC_RemoteApiConfig::load();
    CHECK(loaded.enabled);
    CHECK(loaded.port == 14141);
    CHECK(loaded.token == config.token);
    CHECK_FALSE(loaded.notify);
    CHECK_FALSE(loaded.allowCommands);
    CHECK(loaded.allowedRoots == QStringList{"C:/work", "D:/cad drawings"});
    CHECK(loaded.allowNetworkPaths);

    // a hand-written INI ("Enabled=true") and an invalid port
    LC_SET_ONE("RemoteAPI", "Enabled", QString("false"));
    LC_SET_ONE("RemoteAPI", "Notify", QString("true"));
    LC_SET_ONE("RemoteAPI", "Port", 80);
    loaded = LC_RemoteApiConfig::load();
    CHECK_FALSE(loaded.enabled);
    CHECK(loaded.notify);
    CHECK(loaded.port == LC_RemoteApiConfig::DefaultPort);

    // leave the defaults for the other tests
    LC_RemoteApiConfig defaults;
    defaults.save();
    CHECK(LC_RemoteApiConfig::load().token.isEmpty());
    CHECK_FALSE(LC_RemoteApiConfig::load().enabled);
}

TEST_CASE("Remote API tokens", "[remote-api][core][config]") {
    const QString first = LC_RemoteApiConfig::generateToken();
    const QString second = LC_RemoteApiConfig::generateToken();
    CHECK(first.size() == 32);
    CHECK(first != second);
    for (const QChar c : first) {
        CHECK(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
    }
    const QByteArray token = first.toUtf8();
    CHECK(LC_RemoteApiConfig::tokenEquals(token, token));
    CHECK_FALSE(LC_RemoteApiConfig::tokenEquals(token, second.toUtf8()));
    CHECK_FALSE(LC_RemoteApiConfig::tokenEquals(token, token.left(31)));
    CHECK_FALSE(LC_RemoteApiConfig::tokenEquals(token, token + "0"));
    CHECK_FALSE(LC_RemoteApiConfig::tokenEquals(QByteArray(), QByteArray())); // no token never matches
}

TEST_CASE("Remote API discovery file", "[remote-api][core][config]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath("nested/remote-api.json");
    const qint64 pid = QCoreApplication::applicationPid();
    const QJsonObject content{{"url", "http://127.0.0.1:13140"}, {"token", "secret"}, {"pid", static_cast<double>(pid)}};

    QString error;
    REQUIRE(LC_RemoteApiConfig::writeDiscoveryFile(path, content, &error));
    CHECK(error.isEmpty());
    CHECK(LC_RemoteApiConfig::readDiscoveryFile(path) == content);
#ifdef Q_OS_UNIX
    CHECK(QFile::permissions(path) == (QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser |
                                       QFileDevice::WriteUser));
#endif
    // rewriting replaces it atomically
    QJsonObject second = content;
    second.insert("token", "other");
    REQUIRE(LC_RemoteApiConfig::writeDiscoveryFile(path, second));
    CHECK(LC_RemoteApiConfig::readDiscoveryFile(path).value("token").toString() == "other");

    // another process's file stays
    CHECK_FALSE(LC_RemoteApiConfig::removeDiscoveryFile(path, pid + 1));
    CHECK(QFile::exists(path));
    CHECK(LC_RemoteApiConfig::removeDiscoveryFile(path, pid));
    CHECK_FALSE(QFile::exists(path));
    CHECK(LC_RemoteApiConfig::removeDiscoveryFile(path, pid)); // already gone
    CHECK(LC_RemoteApiConfig::readDiscoveryFile(path).isEmpty());
}

TEST_CASE("Remote API discovery file follows LIBRECAD_SETTINGS_DIR", "[remote-api][core][config]") {
    (void)lc::test::application();
    const QByteArray saved = qgetenv("LIBRECAD_SETTINGS_DIR");
    const bool wasSet = qEnvironmentVariableIsSet("LIBRECAD_SETTINGS_DIR");
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    qputenv("LIBRECAD_SETTINGS_DIR", QDir::toNativeSeparators(dir.path()).toLocal8Bit());
    CHECK(QDir::cleanPath(LC_RemoteApiConfig::settingsSandboxDir()) == QDir::cleanPath(dir.path()));
    CHECK(QDir::cleanPath(LC_RemoteApiConfig::discoveryFilePath()) == QDir::cleanPath(dir.filePath("remote-api.json")));

    qunsetenv("LIBRECAD_SETTINGS_DIR");
    CHECK(LC_RemoteApiConfig::settingsSandboxDir().isEmpty());
    CHECK(LC_RemoteApiConfig::discoveryFilePath().endsWith("/remote-api.json"));
    CHECK_FALSE(QDir::cleanPath(LC_RemoteApiConfig::discoveryFilePath()).startsWith(QDir::cleanPath(dir.path())));

    if (wasSet) {
        qputenv("LIBRECAD_SETTINGS_DIR", saved);
    }
}

// ---------------------------------------------------------------------------
// GUI runner

TEST_CASE("Remote API GUI runner runs a task on the GUI thread", "[remote-api][core][gui]") {
    (void)lc::test::application();
    QThread* guiThread = QThread::currentThread();
    LC_ApiResponse response;
    std::atomic<bool> ranOnGui{false};
    runWhilePumping([&] {
        response = LC_RemoteApiGui::run(
            [&] {
                ranOnGui = QThread::currentThread() == guiThread;
                return LC_ApiResponse::ok({{"depth", LC_RemoteApiGui::depth()}});
            },
            5000, "GET /test");
    });
    CHECK(ranOnGui.load());
    CHECK(response.status == 200);
    CHECK(response.json.value("depth").toInt() == 1);
    CHECK(LC_RemoteApiGui::depth() == 0);
}

TEST_CASE("Remote API GUI runner: a task that times out before it starts never runs", "[remote-api][core][gui]") {
    (void)lc::test::application();
    std::atomic<bool> ran{false};
    int status = 0;
    bool started = true;
    // nothing pumps the GUI thread while the worker waits
    std::thread worker([&] {
        try {
            LC_RemoteApiGui::run([&] {
                ran = true;
                return LC_ApiResponse::ok({});
            }, 100, "GET /slow");
        }
        catch (const LC_ApiError& e) {
            status = e.status;
            started = e.extra.value("started").toBool(true);
            CHECK(e.token == "ui_timeout");
        }
    });
    worker.join();
    CHECK(status == 504);
    CHECK_FALSE(started);
    pumpFor(100); // the queued task finds itself abandoned
    CHECK_FALSE(ran.load());
}

TEST_CASE("Remote API GUI runner: a task that outlives its timeout reports late", "[remote-api][core][gui]") {
    (void)lc::test::application();
    QStringList late;
    LC_RemoteApiGui::setLateTaskNotifier([&late](const QString& route) { late << route; });
    int status = 0;
    bool started = false;
    std::atomic<bool> finished{false};
    runWhilePumping([&] {
        try {
            LC_RemoteApiGui::run([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                finished = true;
                return LC_ApiResponse::ok({});
            }, 100, "POST /api/v1/entities");
        }
        catch (const LC_ApiError& e) {
            status = e.status;
            started = e.extra.value("started").toBool(false);
        }
    });
    pumpFor(600);
    LC_RemoteApiGui::setLateTaskNotifier({});
    CHECK(status == 504);
    CHECK(started);
    CHECK(finished.load());
    CHECK(late == QStringList{"POST /api/v1/entities"});
}

TEST_CASE("Remote API GUI runner gates a task posted inside another one", "[remote-api][core][gui]") {
    (void)lc::test::application();
    std::atomic<bool> firstDone{false};
    std::atomic<bool> secondSawFirstDone{false};
    std::atomic<int> secondDepth{-1};
    LC_ApiResponse second;
    std::thread secondPoster;
    runWhilePumping([&] {
        LC_RemoteApiGui::run([&] {
            // the first task pumps events, as openFile() does; a task posted
            // meanwhile must wait until it has finished
            secondPoster = std::thread([&] {
                second = LC_RemoteApiGui::run([&] {
                    secondSawFirstDone = firstDone.load();
                    secondDepth = LC_RemoteApiGui::depth();
                    return LC_ApiResponse::ok({});
                }, 5000, "GET /second");
            });
            pumpFor(200);
            firstDone = true;
            return LC_ApiResponse::ok({});
        }, 5000, "GET /first");
    });
    runWhilePumping([&] { secondPoster.join(); });
    CHECK(second.status == 200);
    CHECK(secondSawFirstDone.load());
    CHECK(secondDepth.load() == 1);
}

TEST_CASE("Remote API GUI runner refuses while the user has a modal dialog open", "[remote-api][core][gui]") {
    (void)lc::test::application();
    QDialog dialog;
    dialog.setWindowTitle("User dialog");
    dialog.setModal(true); // application modal, shown without blocking
    dialog.show();
    pumpFor(50);
    REQUIRE(QApplication::activeModalWidget() == &dialog);

    std::atomic<bool> ran{false};
    LC_ApiResponse refused;
    LC_ApiResponse allowed;
    runWhilePumping([&] {
        refused = LC_RemoteApiGui::run([&] {
            ran = true;
            return LC_ApiResponse::ok({});
        }, 5000, "POST /x");
        allowed = LC_RemoteApiGui::run([] { return LC_ApiResponse::ok({{"ok", true}}); }, 5000, "GET /api/v1/status",
                                       LC_RemoteApiGui::ModalPolicy::Allow);
    });
    dialog.reject();
    pumpFor(20);

    CHECK_FALSE(ran.load());
    CHECK(refused.status == 409);
    CHECK(refused.json.value("error").toString() == "gui_modal_open");
    CHECK(refused.json.value("window").toString() == "User dialog");
    CHECK(allowed.status == 200);
    CHECK(QApplication::activeModalWidget() == nullptr);
}

TEST_CASE("Remote API GUI runner: abortWaiting answers 503 at once", "[remote-api][core][gui]") {
    (void)lc::test::application();
    int status = 0;
    QElapsedTimer timer;
    timer.start();
    std::thread worker([&] {
        try {
            LC_RemoteApiGui::run([] { return LC_ApiResponse::ok({}); }, 10000, "GET /x");
        }
        catch (const LC_ApiError& e) {
            status = e.status;
            CHECK(e.token == "shutting_down");
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    LC_RemoteApiGui::abortWaiting();
    worker.join();
    CHECK(status == 503);
    CHECK(timer.elapsed() < 2000);
    pumpFor(50);
}

// ---------------------------------------------------------------------------
// Modal guard

TEST_CASE("Remote API modal guard closes message boxes and file dialogs", "[remote-api][core][guard]") {
    (void)lc::test::application();
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, false);
    CHECK_FALSE(LC_RemoteApiModalGuard::isActive());
    {
        LC_RemoteApiModalGuard guard;
        CHECK(LC_RemoteApiModalGuard::isActive());
        CHECK(QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs));

        {
            const DialogWatchdog watchdog(5000);
            const auto answer = QMessageBox::warning(nullptr, "Title", "Overwrite?", QMessageBox::Yes | QMessageBox::No);
            CHECK(answer == QMessageBox::No);
            CHECK_FALSE(watchdog.fired());
        }
        {
            const DialogWatchdog watchdog(5000);
            const auto answer = QMessageBox::question(nullptr, "Save", "Save changes?",
                                                      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
            CHECK(answer == QMessageBox::Cancel); // never the destructive choice
            CHECK_FALSE(watchdog.fired());
        }
        {
            LC_RemoteApiModalGuard inner; // nested: only the innermost records
            const DialogWatchdog watchdog(5000);
            const auto answer = QMessageBox::critical(nullptr, "Inner", "Failed"); // a single OK button
            CHECK(answer == QMessageBox::Ok);
            CHECK_FALSE(watchdog.fired());
            CHECK(inner.skippedDialogs().size() == 1);
        }
        CHECK(QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs)); // the outer scope's value
        {
            // the Qt-drawn dialog, since the guard turned native dialogs off
            const DialogWatchdog watchdog(20000);
            const QString file = QFileDialog::getSaveFileName(nullptr, "Save as", QDir::tempPath(), "*.dxf");
            CHECK(file.isEmpty());
            CHECK_FALSE(watchdog.fired());
        }

        const QJsonArray skipped = guard.skippedDialogs();
        REQUIRE(skipped.size() == 3);
        CHECK(skipped.at(0).toObject().value("class").toString() == "QMessageBox");
        CHECK(skipped.at(0).toObject().value("title").toString() == "Title");
        CHECK(skipped.at(0).toObject().value("text").toString() == "Overwrite?");
        CHECK(skipped.at(1).toObject().value("title").toString() == "Save");
        CHECK(skipped.at(2).toObject().value("class").toString() == "QFileDialog");

        LC_ApiResponse response = LC_ApiResponse::ok({{"done", true}});
        guard.attachTo(response);
        CHECK(response.json.value("skipped_dialogs").toArray().size() == 3);
    }
    CHECK_FALSE(LC_RemoteApiModalGuard::isActive());
    CHECK_FALSE(QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs)); // restored
    pumpFor(20);
}

TEST_CASE("Remote API tasks report the dialogs they closed", "[remote-api][core][guard]") {
    (void)lc::test::application();
    LC_ApiResponse response;
    runWhilePumping([&] {
        response = LC_RemoteApiGui::run([] {
            const DialogWatchdog watchdog(5000);
            QMessageBox::information(nullptr, "Import", "Some entities were skipped");
            return LC_ApiResponse::ok({{"done", true}});
        }, 10000, "POST /api/v1/documents/open");
    });
    CHECK(response.status == 200);
    const QJsonArray skipped = response.json.value("skipped_dialogs").toArray();
    REQUIRE(skipped.size() == 1);
    CHECK(skipped.at(0).toObject().value("text").toString() == "Some entities were skipped");
    CHECK(QApplication::activeModalWidget() == nullptr);
}

// ---------------------------------------------------------------------------
// Preferences tab

TEST_CASE("Remote API Preferences tab edits the RemoteAPI settings", "[remote-api][core][options]") {
    (void)lc::test::application();
    LC_RemoteApiConfig defaults;
    defaults.save();

    LC_RemoteApiOptionsWidget widget(nullptr, nullptr);
    auto* enable = widget.findChild<QCheckBox*>("cbRemoteApiEnable");
    auto* port = widget.findChild<QSpinBox*>("sbRemoteApiPort");
    auto* token = widget.findChild<QLineEdit*>("leRemoteApiToken");
    auto* regenerate = widget.findChild<QPushButton*>("pbRemoteApiRegenerate");
    auto* roots = widget.findChild<QLineEdit*>("leRemoteApiAllowedRoots");
    auto* commands = widget.findChild<QCheckBox*>("cbRemoteApiAllowCommands");
    auto* status = widget.findChild<QLabel*>("lRemoteApiStatus");
    REQUIRE(enable != nullptr);
    REQUIRE(port != nullptr);
    REQUIRE(token != nullptr);
    REQUIRE(regenerate != nullptr);
    REQUIRE(roots != nullptr);
    REQUIRE(commands != nullptr);
    REQUIRE(status != nullptr);

    CHECK_FALSE(enable->isChecked()); // off by default
    CHECK(port->value() == LC_RemoteApiConfig::DefaultPort);
    CHECK(port->minimum() == 1024);
    CHECK(token->text().isEmpty());
    CHECK(token->echoMode() == QLineEdit::Password); // masked
    CHECK(status->text() == "Disabled");

    enable->setChecked(true); // the first enable creates a token
    const QString first = widget.currentConfig().token;
    CHECK(first.size() == 32);
    regenerate->click();
    const QString second = widget.currentConfig().token;
    CHECK(second.size() == 32);
    CHECK(second != first);
    CHECK(LC_RemoteApiConfig::load().token.isEmpty()); // nothing is saved before OK

    port->setValue(14000);
    roots->setText(" C:/a ; ;D:/b ");
    commands->setChecked(false);
    const QString snippet = widget.mcpConfigSnippet();
    const QJsonObject server = QJsonDocument::fromJson(snippet.toUtf8())
                                   .object().value("mcpServers").toObject().value("librecad").toObject();
    CHECK(server.value("command").toString() == "uvx");
    const QJsonArray args = server.value("args").toArray();
    REQUIRE(args.size() == 3);
    CHECK(args.at(1).toString().startsWith("git+https://github.com/ShiftSad/LibreCAD-mcp@v"));
    CHECK(args.at(1).toString().endsWith(QString("-mcp.%1#subdirectory=mcp").arg(LC_RemoteApiController::mcpBuild())));
    CHECK_FALSE(args.at(1).toString().contains("alpha")); // the tag has no pre-release label
    CHECK(args.at(2).toString() == "librecad-mcp");
    CHECK(server.value("env").toObject().value("LIBRECAD_API_TOKEN").toString() == second);
    CHECK(server.value("env").toObject().value("LIBRECAD_API_URL").toString() == "http://127.0.0.1:14000");

    widget.save(); // QG_DlgOptionsGeneral::ok()
    const LC_RemoteApiConfig saved = LC_RemoteApiConfig::load();
    CHECK(saved.enabled);
    CHECK(saved.port == 14000);
    CHECK(saved.token == second);
    CHECK(saved.allowedRoots == QStringList{"C:/a", "D:/b"});
    CHECK_FALSE(saved.allowCommands);

    defaults.save();
}
