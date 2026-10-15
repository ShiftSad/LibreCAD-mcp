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

// Tests of the documents component of the Remote API (design section 5.1,
// "Documents"): format table, path policy, and the routes of
// lcRegisterDocumentRoutes() run through a router against the real main
// window, the way the server runs them: dispatched from another thread while
// the GUI thread runs the handlers. Every test checks that no modal dialog
// showed up.

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>

#include "lc_actiontestsupport.h"
#include "lc_graphicviewport.h"
#include "lc_remoteapiappaccess.h"
#include "lc_remoteapicontext.h"
#include "lc_remoteapiformats.h"
#include "lc_remoteapipathpolicy.h"
#include "lc_remoteapirenderer.h"
#include "lc_remoteapirouter.h"
#include "lc_remoteapiroutes.h"
#include "lc_remoteapitypes.h"
#include "lc_testsettingsisolation.h"
#include "qc_applicationwindow.h"
#include "qc_mdiwindow.h"
#include "qg_graphicview.h"
#include "rs_debug.h"
#include "rs_document.h"
#include "rs_fileio.h"
#include "rs_graphic.h"
#include "rs_line.h"
#include "rs_settings.h"
#include "rs_system.h"

namespace {

using Access = LC_RemoteApiPathPolicy::Access;

// ------------------------------------------------------------ main window

std::unique_ptr<QC_ApplicationWindow>* windowOwner = nullptr;

class DocumentsWindowCleanup final : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testRunEnded(const Catch::TestRunStats&) override {
        // as main(): destroy the widgets before Qt's static resources go
        if (windowOwner != nullptr) {
            windowOwner->reset();
        }
    }
};

CATCH_REGISTER_LISTENER(DocumentsWindowCleanup)

void settle(const int milliseconds = 0) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

QC_ApplicationWindow& mainWindow() {
    REQUIRE(lc::test::application() != nullptr);
    REQUIRE(lc::test::SettingsIsolation::isActive());
    static QC_ApplicationWindow* window = [] {
        qApp->setQuitOnLastWindowClosed(false);
        RS_DEBUG->setLevel(RS_Debug::D_NOTHING);
        RS_SYSTEM->init("LibreCAD", "3.0", "librecad", QString(LIBRECAD_SOURCE_DIR) + "/unix");
        QSettings settings;
        settings.setValue("Startup/FirstLoad", true);
        settings.setValue("Defaults/AutoBackupDocument", false);
        settings.setValue("Paths/OtherSettingsDir", lc::test::SettingsIsolation::directory());
        windowOwner = &QC_ApplicationWindow::getAppWindow();
        QC_ApplicationWindow* created = windowOwner->get();
        created->resize(1000, 700);
        created->prepareWindowForShow();
        created->show();
        settle(100);
        return created;
    }();
    return *window;
}

// ------------------------------------------------------------ requests

QString errorToken(const LC_ApiResponse& response) {
    return response.json.value(QStringLiteral("error")).toString();
}

/**
 * The routes of the documents component on a router, with the request
 * plumbing of the server: dispatch() runs on a worker thread while this (GUI)
 * thread runs the posted handlers. A watchdog rejects any modal dialog and
 * records it, so a dialog fails the test instead of hanging it.
 */
struct Api {
    QC_ApplicationWindow& app = mainWindow();
    LC_RemoteApiContext ctx{&app};
    LC_RemoteApiRouter router;
    QStringList modals;
    QTimer watchdog;

    Api() {
        lcRegisterDocumentRoutes(router);
        QObject::connect(&watchdog, &QTimer::timeout, [this] {
            if (QWidget* modal = QApplication::activeModalWidget()) {
                modals.append(QString::fromLatin1(modal->metaObject()->className()) + QStringLiteral(": ")
                              + modal->windowTitle());
                if (auto* dialog = qobject_cast<QDialog*>(modal)) {
                    dialog->reject();
                }
                else {
                    modal->close();
                }
            }
        });
        watchdog.start(100);
        closeAll();
    }

    ~Api() {
        closeAll();
        watchdog.stop();
    }

    void closeAll() {
        for (QC_MDIWindow* window : ctx.documentWindows()) {
            LC_RemoteApiAppAccess::close(&app, window);
        }
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        settle();
    }

    LC_ApiResponse send(const QByteArray& method, const QString& target, const QJsonObject& body = {}) {
        LC_ApiRequest request;
        request.method = method;
        const qsizetype question = target.indexOf(QLatin1Char('?'));
        request.path = question < 0 ? target : target.left(question);
        if (question >= 0) {
            const QUrlQuery query(target.mid(question + 1));
            for (const auto& [key, value] : query.queryItems(QUrl::FullyDecoded)) {
                request.query.insert(key, value);
            }
        }
        request.headers.insert("host", "127.0.0.1:13140");
        if (!body.isEmpty()) {
            request.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
        }

        LC_ApiResponse response;
        std::atomic<bool> done{false};
        std::thread worker([&] {
            try {
                response = router.dispatch(request, ctx);
            }
            catch (const LC_ApiError& error) {
                response = error.toResponse();
            }
            catch (const std::exception& error) {
                response = LC_ApiError(500, QStringLiteral("internal"), QString::fromUtf8(error.what())).toResponse();
            }
            done = true;
        });
        while (!done.load()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        worker.join();
        settle();
        // The real GUI runner's modal guard closes a dialog that a route shows
        // before the watchdog can see it, and reports it in the answer: count
        // those as dialogs too.
        for (const QJsonValue& skipped : response.json.value(QStringLiteral("skipped_dialogs")).toArray()) {
            const QJsonObject dialog = skipped.toObject();
            modals.append(dialog.value(QStringLiteral("class")).toString() + QStringLiteral(": ")
                          + dialog.value(QStringLiteral("title")).toString());
        }
        for (const auto& [key, value] : response.extraHeaders) {
            if (key.compare("X-LibreCAD-Skipped-Dialogs", Qt::CaseInsensitive) == 0) {
                modals.append(QStringLiteral("%1 dialog(s) closed while rendering").arg(QString::fromLatin1(value)));
            }
        }
        return response;
    }

    LC_ApiResponse get(const QString& target) {
        return send("GET", target);
    }

    LC_ApiResponse post(const QString& target, const QJsonObject& body = {}) {
        return send("POST", target, body);
    }

    QC_MDIWindow* window(const qint64 id) {
        for (QC_MDIWindow* candidate : ctx.documentWindows()) {
            if (candidate->getId() == id) {
                return candidate;
            }
        }
        return nullptr;
    }

    /** A new drawing through the route; returns its window. */
    QC_MDIWindow* newDocument() {
        const LC_ApiResponse response = post(QStringLiteral("/api/v1/documents/new"));
        REQUIRE(response.status == 201);
        QC_MDIWindow* created = window(response.json.value("document").toObject().value("id").toInteger());
        REQUIRE(created != nullptr);
        return created;
    }
};

QString docQuery(const QC_MDIWindow* window) {
    return QStringLiteral("?doc=%1").arg(window->getId());
}

/** Adds a line in one undo cycle, as an interactive action does. */
void addLine(QC_MDIWindow* window, const RS_Vector& start, const RS_Vector& end) {
    RS_Document* document = window->getDocument();
    document->undoableModify(window->getGraphicView()->getViewPort(), [&](LC_DocumentModificationBatch& batch) {
        batch += new RS_Line(document, start, end);
        return true;
    });
}

int liveEntities(const QC_MDIWindow* window) {
    int count = 0;
    for (RS_Entity* entity : *window->getDocument()) {
        count += (entity != nullptr && !entity->isDeleted()) ? 1 : 0;
    }
    return count;
}

/** The error an LC_ApiError-throwing call gives, as {status, token}. */
std::pair<int, QString> errorOf(const std::function<void()>& call) {
    try {
        call();
    }
    catch (const LC_ApiError& error) {
        return {error.status, error.token};
    }
    return {0, QString()};
}

QByteArray fileStart(const QString& path, const int length) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.read(length);
}

bool visibleMessageBox() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (qobject_cast<QMessageBox*>(widget) != nullptr && widget->isVisible()) {
            return true;
        }
    }
    return false;
}

QByteArray header(const LC_ApiResponse& response, const QByteArray& name) {
    for (const auto& [key, value] : response.extraHeaders) {
        if (key.compare(name, Qt::CaseInsensitive) == 0) {
            return value;
        }
    }
    return {};
}

const QByteArray PngSignature("\x89PNG\r\n\x1a\n", 8);

} // namespace

// ================================================================= formats

TEST_CASE("Remote API formats: every listed version is one LibreCAD writes", "[remote-api][documents]") {
    for (const QString format : {"dxf", "dwg"}) {
        CAPTURE(format);
        const auto versions = LC_RemoteApiFormats::versions(format);
        REQUIRE_FALSE(versions.isEmpty());
        for (const auto& version : versions) {
            CAPTURE(version.name);
            CHECK(RS_FileIO::instance()->canExport(version.type));
            CHECK(LC_RemoteApiFormats::resolve(format, version.name) == version.type);
            REQUIRE(LC_RemoteApiFormats::find(version.type) != nullptr);
            CHECK(LC_RemoteApiFormats::find(version.type)->name == version.name);
            CHECK_FALSE(version.label.isEmpty());
        }
    }

    CHECK(LC_RemoteApiFormats::resolve("dxf", "") == RS2::FormatDXFRW);
    CHECK(LC_RemoteApiFormats::resolve("DXF", "r12") == RS2::FormatDXFRW12);
    CHECK(LC_RemoteApiFormats::resolve("dxf", "AC1015") == RS2::FormatDXFRW2000);
    CHECK(LC_RemoteApiFormats::resolve("dwg", "") == RS2::FormatDWG2018);
    CHECK(LC_RemoteApiFormats::resolve("dwg", "2026") == RS2::FormatDWG2018);
    CHECK(LC_RemoteApiFormats::resolve("dwg", "AC1032") == RS2::FormatDWG2018);
    CHECK(LC_RemoteApiFormats::resolve("dwg", "R2000") == RS2::FormatDWG);
    CHECK(LC_RemoteApiFormats::resolve("dwg", "2007") == RS2::FormatDWG2007);

    CHECK(errorOf([] { LC_RemoteApiFormats::resolve("dwg", "1999"); }) == std::pair<int, QString>{422, "bad_param"});
    CHECK(errorOf([] { LC_RemoteApiFormats::resolve("pdf", ""); })
          == std::pair<int, QString>{422, "unsupported_format"});
    try {
        LC_RemoteApiFormats::resolve("dwg", "R14");
        FAIL("R14 DWG is not writable");
    }
    catch (const LC_ApiError& error) {
        CHECK(error.extra.value("param").toString() == "version");
        CHECK(error.extra.value("allowed").toArray().contains(QJsonValue("2018")));
    }

    const QJsonObject dwg2018 = LC_RemoteApiFormats::describe(RS2::FormatDWG2018).toObject();
    CHECK(dwg2018.value("format").toString() == "dwg");
    CHECK(dwg2018.value("version").toString() == "2018");
    CHECK(dwg2018.value("label").toString() == "AutoCAD 2018-2026 (AC1032)");
    CHECK(LC_RemoteApiFormats::describe(RS2::FormatDXFRW).toObject().value("version").toString() == "2007");
    CHECK(LC_RemoteApiFormats::describe(RS2::FormatUnknown).isNull());
    CHECK(LC_RemoteApiFormats::describe(RS2::FormatLFF).toObject().value("format").toString() == "other");

    const QJsonObject table = LC_RemoteApiFormats::toJson();
    CHECK(table.value("open").toArray().contains(QJsonValue(".dwg")));
    CHECK(table.value("open").toArray().contains(QJsonValue(".jww")));
    CHECK(table.value("export").toArray().contains(QJsonValue("pdf")));
    QJsonObject dwgSave;
    for (const QJsonValue& entry : table.value("save").toArray()) {
        if (entry.toObject().value("format").toString() == "dwg") {
            dwgSave = entry.toObject();
        }
    }
    CHECK(dwgSave.value("default").toString() == "2018");
    CHECK(dwgSave.value("labels").toObject().value("2018").toString() == "AutoCAD 2018-2026 (AC1032)");
    CHECK(dwgSave.value("aliases").toObject().value("2026").toString() == "2018");
    CHECK(dwgSave.value("aliases").toObject().value("AC1032").toString() == "2018");

    CHECK(LC_RemoteApiFormats::openFormat(QStringLiteral("C:/nowhere/drawing.jww")) == RS2::FormatJWW);
    CHECK(LC_RemoteApiFormats::openFormat(QStringLiteral("C:/nowhere/notes.txt")) == RS2::FormatUnknown);
}

// ================================================================= path policy

TEST_CASE("Remote API path policy refuses unsafe paths", "[remote-api][documents]") {
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const QDir dir(temp.path());
    REQUIRE(dir.mkpath("inside"));
    REQUIRE(dir.mkpath("outside"));
    const QString existing = dir.filePath("inside/existing.dxf");
    {
        QFile file(existing);
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write("0\nEOF\n");
    }
    using Error = std::pair<int, QString>;
    const LC_RemoteApiPathPolicy open(LC_RemoteApiConfig{});

    SECTION("relative and drive-relative paths") {
        CHECK(errorOf([&] { open.check("drawing.dxf", Access::Read); }) == Error{400, "bad_path"});
        CHECK(errorOf([&] { open.check("", Access::Write); }) == Error{400, "bad_path"});
#ifdef Q_OS_WIN
        CHECK(errorOf([&] { open.check("C:drawing.dxf", Access::Write); }) == Error{400, "bad_path"});
        CHECK(errorOf([&] { open.check("/drawing.dxf", Access::Write); }) == Error{400, "bad_path"});
#endif
    }

    SECTION("network and device paths need AllowNetworkPaths") {
        for (const char* path : {"\\\\server\\share\\a.dxf", "//server/share/a.dxf", "\\\\?\\C:\\a.dxf",
                                 "\\\\.\\PhysicalDrive0"}) {
            CAPTURE(path);
            CHECK(errorOf([&] { open.check(path, Access::Read); }) == Error{403, "path_not_allowed"});
        }
        CHECK(LC_RemoteApiPathPolicy::isNetworkPath("\\\\host\\x"));
        CHECK_FALSE(LC_RemoteApiPathPolicy::isNetworkPath(existing));
    }

#ifdef Q_OS_WIN
    SECTION("device namespace paths are refused even with AllowNetworkPaths") {
        LC_RemoteApiConfig config;
        config.allowNetworkPaths = true;
        const LC_RemoteApiPathPolicy network(config);
        const QString native = QDir::toNativeSeparators(existing);
        // "\\?\C:\...\existing.dxf:q.pdf" would write a hidden stream of a local file
        const QStringList devicePaths{QStringLiteral("\\\\?\\") + native + QStringLiteral(":q.pdf"),
                                      QStringLiteral("\\\\.\\") + native + QStringLiteral(":q.pdf"),
                                      QStringLiteral("\\\\?\\") + native, QStringLiteral("//?/") + existing,
                                      QStringLiteral("\\\\?\\UNC\\server\\share\\a.dxf"), QStringLiteral("\\??\\") + native};
        for (const QString& path : devicePaths) {
            CAPTURE(path);
            CHECK(errorOf([&] { network.check(path, Access::Write, {".pdf", ".dxf"}, true); }) == Error{403, "path_not_allowed"});
        }
        // a stream on a share is refused like a local one
        CHECK(errorOf([&] { network.check("\\\\server\\share\\host.txt:x.pdf", Access::Write, {".pdf"}); })
              == Error{400, "bad_path"});
        CHECK(errorOf([&] { network.check("//server/share/host.txt:x.pdf", Access::Write, {".pdf"}); })
              == Error{400, "bad_path"});
    }
#endif

    SECTION("allowed roots") {
        LC_RemoteApiConfig config;
        config.allowedRoots = {dir.filePath("inside")};
        const LC_RemoteApiPathPolicy policy(config);
        CHECK(policy.check(dir.filePath("inside/new.dxf"), Access::Write) == QDir::cleanPath(dir.filePath("inside/new.dxf")));
        CHECK(errorOf([&] { policy.check(dir.filePath("outside/new.dxf"), Access::Write); })
              == Error{403, "path_not_allowed"});
        CHECK(errorOf([&] { policy.check(dir.filePath("inside/../outside/new.dxf"), Access::Write); })
              == Error{403, "path_not_allowed"});
        // a prefix of the folder name is not the folder
        LC_RemoteApiConfig prefix;
        prefix.allowedRoots = {dir.filePath("ins")};
        CHECK(errorOf([&] { LC_RemoteApiPathPolicy(prefix).check(dir.filePath("inside/new.dxf"), Access::Write); })
              == Error{403, "path_not_allowed"});
        // save in place checks the location too
        CHECK(errorOf([&] { policy.check(dir.filePath("outside/x.dxf"), Access::WriteInPlace); })
              == Error{403, "path_not_allowed"});
    }

    SECTION("extensions, folders and existing files") {
        CHECK(errorOf([&] { open.check(dir.filePath("inside/a.dwg"), Access::Write, {".dxf"}); })
              == Error{422, "extension_mismatch"});
        CHECK(errorOf([&] { open.check(dir.filePath("inside/a"), Access::Write, {".dxf"}); })
              == Error{422, "extension_mismatch"});
        CHECK_FALSE(open.check(dir.filePath("inside/UPPER.DXF"), Access::Write, {".dxf"}).isEmpty());
        CHECK(errorOf([&] { open.check(dir.filePath("missing/a.dxf"), Access::Write); }) == Error{400, "bad_path"});
        CHECK(errorOf([&] { open.check(dir.filePath("inside"), Access::Write); }) == Error{400, "bad_path"});
        CHECK(errorOf([&] { open.check(dir.filePath("inside/none.dxf"), Access::Read); })
              == Error{404, "file_not_found"});
        CHECK(errorOf([&] { open.check(existing, Access::Write, {".dxf"}); }) == Error{409, "file_exists"});
        CHECK(open.check(existing, Access::Write, {".dxf"}, true) == QDir::cleanPath(existing));
        CHECK(open.check(existing, Access::Read) == QDir::cleanPath(existing));
    }

    SECTION("control characters: a NUL would cut the name the system opens") {
        // "<dir>/run.bat\0.dxf" would pass the extension check as .dxf and write run.bat
        const QString nul = dir.filePath("inside/run.bat") + QChar(0) + QStringLiteral(".dxf");
        CHECK(errorOf([&] { open.check(nul, Access::Write, {".dxf"}); }) == Error{400, "bad_path"});
        // nor can it get past the overwrite rule
        const QString overwrite = existing + QChar(0) + QStringLiteral(".dxf");
        CHECK(errorOf([&] { open.check(overwrite, Access::Write, {".dxf"}); }) == Error{400, "bad_path"});
        CHECK(errorOf([&] { open.check(existing + QChar(0), Access::Read); }) == Error{400, "bad_path"});
        for (const QChar c : {QChar(0x01), QChar(0x0a), QChar(0x1f), QChar(0x7f)}) {
            CHECK(errorOf([&] { open.check(dir.filePath("inside/a") + c + QStringLiteral("b.dxf"), Access::Write); })
                  == Error{400, "bad_path"});
        }
        CHECK_FALSE(QFileInfo::exists(dir.filePath("inside/run.bat")));
#ifdef Q_OS_WIN
        // an alternate data stream of an existing file is not a file of its own
        CHECK(errorOf([&] { open.check(existing + QStringLiteral(":hidden.dxf"), Access::Write, {".dxf"}); })
              == Error{400, "bad_path"});
        // nor is a device name a file
        for (const char* device : {"inside/COM1.dxf", "inside/nul.dxf", "inside/CONIN$.dxf", "inside/Lpt9.svg"}) {
            CAPTURE(device);
            CHECK(errorOf([&] { open.check(dir.filePath(QString::fromLatin1(device)), Access::Write); }) == Error{400, "bad_path"});
        }
        CHECK_FALSE(open.check(dir.filePath("inside/console.dxf"), Access::Write).isEmpty());
#endif
    }

#ifdef Q_OS_WIN
    SECTION("a directory junction inside an allowed folder does not lead out of it") {
        const QString junction = QDir::toNativeSeparators(dir.filePath("inside/jn"));
        const QString target = QDir::toNativeSeparators(dir.filePath("outside"));
        QProcess mklink;
        mklink.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"), junction, target});
        if (!mklink.waitForFinished(10000) || mklink.exitCode() != 0 || !QFileInfo(junction).exists()) {
            SKIP("mklink /J is not available here");
        }
        {
            QFile secret(dir.filePath("outside/secret.dxf"));
            REQUIRE(secret.open(QIODevice::WriteOnly));
            secret.write("0\nEOF\n");
        }
        LC_RemoteApiConfig config;
        config.allowedRoots = {dir.filePath("inside")};
        const LC_RemoteApiPathPolicy policy(config);
        CHECK(errorOf([&] { policy.check(dir.filePath("inside/jn/escaped.dxf"), Access::Write); })
              == Error{403, "path_not_allowed"});
        CHECK(errorOf([&] { policy.check(dir.filePath("inside/jn/secret.dxf"), Access::Read); })
              == Error{403, "path_not_allowed"});
        CHECK(errorOf([&] { policy.check(dir.filePath("inside/jn/deeper/new.dxf"), Access::Write); })
              == Error{403, "path_not_allowed"});
        // the folder itself is still usable
        CHECK_FALSE(policy.check(dir.filePath("inside/plain.dxf"), Access::Write).isEmpty());
        // a path over MAX_PATH through the junction is resolved too (Qt's writers reach it)
        QString deep;
        for (int i = 0; i < 6; ++i) {
            deep += QStringLiteral("/") + QString(40, QChar(u'a' + i));
        }
        REQUIRE(QDir().mkpath(dir.filePath("outside") + deep));
        REQUIRE(QDir().mkpath(dir.filePath("inside") + deep));
        const QString longEscape = dir.filePath("inside/jn") + deep + QStringLiteral("/escaped.png");
        REQUIRE(longEscape.size() > 300);
        CHECK(errorOf([&] { policy.check(longEscape, Access::Write, {".png"}); }) == Error{403, "path_not_allowed"});
        CHECK(errorOf([&] { policy.check(dir.filePath("inside/jn") + deep, Access::Read); }) == Error{403, "path_not_allowed"});
        const QString longInside = dir.filePath("inside") + deep + QStringLiteral("/kept.png");
        REQUIRE(longInside.size() > 260);
        CHECK(policy.check(longInside, Access::Write, {".png"}) == QDir::cleanPath(longInside));
        // an allowed root that is itself a junction is resolved the same way
        LC_RemoteApiConfig viaJunction;
        viaJunction.allowedRoots = {dir.filePath("inside/jn")};
        CHECK_FALSE(LC_RemoteApiPathPolicy(viaJunction).check(dir.filePath("outside/new.dxf"), Access::Write).isEmpty());
        QDir().rmdir(junction); // removes the junction, not the folder it points at
    }
#endif

    SECTION("same file") {
        CHECK(LC_RemoteApiPathPolicy::sameFile(existing, dir.filePath("inside/../inside/existing.dxf")));
#ifdef Q_OS_WIN
        CHECK(LC_RemoteApiPathPolicy::sameFile(existing, existing.toUpper()));
#endif
        CHECK_FALSE(LC_RemoteApiPathPolicy::sameFile(existing, dir.filePath("inside/other.dxf")));
        CHECK_FALSE(LC_RemoteApiPathPolicy::sameFile(existing, QString()));
    }
}

// ================================================================= documents

TEST_CASE("Remote API documents: new, list, activate and close never ask", "[remote-api][documents][app]") {
    Api api;
    const LC_ApiResponse none = api.get("/api/v1/documents");
    REQUIRE(none.status == 200);
    CHECK(none.json.value("documents").toArray().isEmpty());
    CHECK(none.json.value("active").isNull());
    CHECK(errorToken(api.get("/api/v1/drawing")) == "no_document");

    const LC_ApiResponse first = api.post("/api/v1/documents/new");
    REQUIRE(first.status == 201);
    const QJsonObject firstDoc = first.json.value("document").toObject();
    CHECK(firstDoc.value("active").toBool());
    CHECK(firstDoc.value("path").isNull());
    CHECK_FALSE(firstDoc.value("modified").toBool());
    CHECK(firstDoc.value("entity_count").toInt() == 0);
    CHECK(firstDoc.value("format").toObject().value("format").toString() == "dxf");
    CHECK_FALSE(firstDoc.value("name").toString().isEmpty());

    const LC_ApiResponse second = api.post("/api/v1/documents/new", {{"activate", false}});
    REQUIRE(second.status == 201);
    const qint64 firstId = firstDoc.value("id").toInteger();
    const qint64 secondId = second.json.value("document").toObject().value("id").toInteger();
    CHECK(firstId != secondId);

    const LC_ApiResponse list = api.get("/api/v1/documents");
    REQUIRE(list.status == 200);
    CHECK(list.json.value("documents").toArray().size() == 2);
    CHECK(list.json.value("active").toInteger() == firstId);

    const LC_ApiResponse activated = api.post(QStringLiteral("/api/v1/documents/activate?doc=%1").arg(secondId));
    REQUIRE(activated.status == 200);
    CHECK(activated.json.value("document").toObject().value("active").toBool());
    CHECK(api.get("/api/v1/documents").json.value("active").toInteger() == secondId);

    QC_MDIWindow* secondWindow = api.window(secondId);
    REQUIRE(secondWindow != nullptr);
    addLine(secondWindow, {0, 0}, {10, 0});
    REQUIRE(secondWindow->isModified());
    const LC_ApiResponse refused = api.post(QStringLiteral("/api/v1/documents/close?doc=%1").arg(secondId));
    CHECK(refused.status == 409);
    CHECK(errorToken(refused) == "unsaved_changes");
    REQUIRE(api.window(secondId) != nullptr);

    const LC_ApiResponse closed = api.post(QStringLiteral("/api/v1/documents/close?doc=%1").arg(secondId),
                                           {{"discard_changes", true}});
    REQUIRE(closed.status == 200);
    CHECK(closed.json.value("closed").toBool());
    CHECK(closed.json.value("discarded_changes").toBool());
    CHECK(api.window(secondId) == nullptr);

    CHECK(errorToken(api.post("/api/v1/documents/close?doc=999999")) == "unknown_document");
    CHECK(errorToken(api.post("/api/v1/documents/new", {{"bogus", 1}})) == "unknown_field");

    const LC_ApiResponse closeFirst = api.post(QStringLiteral("/api/v1/documents/close?doc=%1").arg(firstId));
    REQUIRE(closeFirst.status == 200);
    CHECK_FALSE(closeFirst.json.value("discarded_changes").toBool());
    CHECK(api.ctx.documentWindows().isEmpty());
    CHECK(api.modals.isEmpty());
}

TEST_CASE("Remote API documents: open reads a drawing, and a corrupt DWG fails without a box",
          "[remote-api][documents][app]") {
    Api api;
    const QString fixture = QString(LIBRECAD_TEST_DIR) + "/dxf/embedded_mtext_external_r2018.dxf";
    REQUIRE(QFileInfo::exists(fixture));

    const LC_ApiResponse opened = api.post("/api/v1/documents/open", {{"path", fixture}});
    REQUIRE(opened.status == 201);
    const QJsonObject document = opened.json.value("document").toObject();
    CHECK(LC_RemoteApiPathPolicy::sameFile(document.value("path").toString(), fixture));
    CHECK(document.value("format").toObject().value("format").toString() == "dxf");
    CHECK(document.value("format").toObject().value("version").toString() == "2018");
    CHECK(document.value("entity_count").toInt() >= 1);
    CHECK_FALSE(document.value("modified").toBool());
    CHECK(document.value("active").toBool());

    const LC_ApiResponse again = api.post("/api/v1/documents/open", {{"path", fixture}});
    REQUIRE(again.status == 200);
    CHECK(again.json.value("already_open").toBool());
    CHECK(again.json.value("document").toObject().value("id") == document.value("id"));

    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    CHECK(errorToken(api.post("/api/v1/documents/open", {{"path", "relative.dxf"}})) == "bad_path");
    CHECK(errorToken(api.post("/api/v1/documents/open", {{"path", temp.filePath("missing.dxf")}})) == "file_not_found");
    CHECK(errorToken(api.post("/api/v1/documents/open", {})) == "missing_param");
    {
        QFile notes(temp.filePath("notes.txt"));
        REQUIRE(notes.open(QIODevice::WriteOnly));
        notes.write("not a drawing");
    }
    const LC_ApiResponse unsupported = api.post("/api/v1/documents/open", {{"path", temp.filePath("notes.txt")}});
    CHECK(unsupported.status == 422);
    CHECK(errorToken(unsupported) == "unsupported_format");

    const QString corrupt = temp.filePath("corrupt.dwg");
    {
        QFile file(corrupt);
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write("AC1032");
        file.write(QByteArray(4096, '\x5a'));
    }
    const qsizetype windowsBefore = api.ctx.documentWindows().size();
    const LC_ApiResponse failed = api.post("/api/v1/documents/open", {{"path", corrupt}});
    CHECK(failed.status == 422);
    CHECK(errorToken(failed) == "import_failed");
    CHECK(api.ctx.documentWindows().size() == windowsBefore);
    CHECK_FALSE(visibleMessageBox());
    CHECK(api.modals.isEmpty());
}

TEST_CASE("Remote API documents: save_as DWG 2018, save in place, then reopen", "[remote-api][documents][app]") {
    Api api;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const QString dwgPath = QDir::cleanPath(temp.filePath("plan.dwg"));

    QC_MDIWindow* window = api.newDocument();
    addLine(window, {0, 0}, {100, 0});
    const LC_ApiResponse unnamed = api.post("/api/v1/documents/save" + docQuery(window));
    CHECK(unnamed.status == 409);
    CHECK(errorToken(unnamed) == "document_unnamed");

    CHECK(errorToken(api.post("/api/v1/documents/save_as" + docQuery(window),
                              {{"path", temp.filePath("plan.dxf")}, {"format", "dwg"}})) == "extension_mismatch");
    CHECK(errorToken(api.post("/api/v1/documents/save_as" + docQuery(window),
                              {{"path", dwgPath}, {"format", "dwg"}, {"version", "1999"}})) == "bad_param");
    CHECK(errorToken(api.post("/api/v1/documents/save_as" + docQuery(window),
                              {{"path", temp.filePath("plan.pdf")}, {"format", "pdf"}})) == "unsupported_format");

    const LC_ApiResponse saved = api.post("/api/v1/documents/save_as" + docQuery(window),
                                          {{"path", dwgPath}, {"format", "dwg"}, {"version", "2026"}});
    REQUIRE(saved.status == 200);
    CHECK(saved.json.value("format").toString() == "dwg");
    CHECK(saved.json.value("version").toString() == "2018");
    CHECK(saved.json.value("label").toString() == "AutoCAD 2018-2026 (AC1032)");
    CHECK(saved.json.value("left_out").isArray());
    CHECK(LC_RemoteApiPathPolicy::sameFile(saved.json.value("path").toString(), dwgPath));
    CHECK_FALSE(saved.json.value("document").toObject().value("modified").toBool());
    CHECK(LC_RemoteApiPathPolicy::sameFile(window->getFileName(), dwgPath));
    CHECK(window->windowTitle().contains("plan.dwg"));
    CHECK(fileStart(dwgPath, 6) == "AC1032");

    const LC_ApiResponse exists = api.post("/api/v1/documents/save_as" + docQuery(window),
                                           {{"path", dwgPath}, {"format", "dwg"}});
    CHECK(exists.status == 409);
    CHECK(errorToken(exists) == "file_exists");
    CHECK(api.post("/api/v1/documents/save_as" + docQuery(window),
                   {{"path", dwgPath}, {"format", "dwg"}, {"overwrite", true}})
              .status == 200);

    addLine(window, {0, 0}, {0, 50});
    REQUIRE(window->isModified());
    const LC_ApiResponse inPlace = api.post("/api/v1/documents/save" + docQuery(window));
    REQUIRE(inPlace.status == 200);
    CHECK(inPlace.json.value("version").toString() == "2018");
    CHECK_FALSE(window->isModified());

    REQUIRE(api.post("/api/v1/documents/close" + docQuery(window)).status == 200);
    const LC_ApiResponse reopened = api.post("/api/v1/documents/open", {{"path", dwgPath}});
    REQUIRE(reopened.status == 201);
    QC_MDIWindow* again = api.window(reopened.json.value("document").toObject().value("id").toInteger());
    REQUIRE(again != nullptr);
    CHECK(again->getGraphic()->getFormatType() == RS2::FormatDWG2018);
    CHECK(liveEntities(again) == 2);
    CHECK(reopened.json.value("document").toObject().value("format").toObject().value("version").toString() == "2018");
    CHECK(api.modals.isEmpty());
}

TEST_CASE("Remote API documents: export writes copies and leaves the drawing as it was",
          "[remote-api][documents][app]") {
    Api api;
    QTemporaryDir temp;
    REQUIRE(temp.isValid());
    const QString basePath = QDir::cleanPath(temp.filePath("base.dxf"));

    QC_MDIWindow* window = api.newDocument();
    addLine(window, {0, 0}, {200, 100});
    REQUIRE(api.post("/api/v1/documents/save_as" + docQuery(window), {{"path", basePath}, {"format", "dxf"}}).status
            == 200);
    RS_Graphic* graphic = window->getGraphic();
    REQUIRE_FALSE(graphic->isModified());
    const RS2::FormatType formatBefore = graphic->getFormatType();

    SECTION("DXF R12 copy") {
        const QString copy = temp.filePath("copy12.dxf");
        const LC_ApiResponse exported = api.post("/api/v1/documents/export" + docQuery(window),
                                                 {{"path", copy}, {"format", "dxf"}, {"version", "R12"}});
        REQUIRE(exported.status == 200);
        CHECK(exported.json.value("version").toString() == "R12");
        CHECK(exported.json.value("bytes").toInteger() > 0);
        QFile file(copy);
        REQUIRE(file.open(QIODevice::ReadOnly));
        CHECK(file.readAll().contains("AC1009"));
        CHECK(LC_RemoteApiPathPolicy::sameFile(graphic->getFilename(), basePath));
        CHECK(graphic->getFormatType() == formatBefore);
        CHECK_FALSE(graphic->isModified());
    }

    SECTION("not over the drawing's own file") {
        const LC_ApiResponse own = api.post("/api/v1/documents/export" + docQuery(window),
                                            {{"path", basePath}, {"format", "dxf"}, {"overwrite", true}});
        CHECK(own.status == 409);
        CHECK(errorToken(own) == "file_open_in_other_window");
    }

    SECTION("PNG and SVG") {
        const QString png = temp.filePath("picture.png");
        const LC_ApiResponse exported = api.post(
            "/api/v1/documents/export" + docQuery(window),
            {{"path", png}, {"format", "png"}, {"width", 400}, {"height", 300}, {"background", "black"}});
        REQUIRE(exported.status == 200);
        CHECK(fileStart(png, 8) == PngSignature);
        const QImage image(png);
        CHECK(image.size() == QSize(400, 300));

        const QString svg = temp.filePath("picture.svg");
        REQUIRE(api.post("/api/v1/documents/export" + docQuery(window), {{"path", svg}, {"format", "svg"}}).status
                == 200);
        QFile file(svg);
        REQUIRE(file.open(QIODevice::ReadOnly));
        CHECK(file.readAll().contains("<svg"));
        CHECK_FALSE(graphic->isModified());

        CHECK(errorToken(api.post("/api/v1/documents/export" + docQuery(window),
                                  {{"path", temp.filePath("x.png")}, {"format", "png"}, {"version", "2018"}}))
              == "bad_param");
        CHECK(errorToken(api.post("/api/v1/documents/export" + docQuery(window),
                                  {{"path", temp.filePath("x.bmp")}, {"format", "bmp"}})) == "unsupported_format");
        CHECK(errorToken(api.post("/api/v1/documents/export" + docQuery(window),
                                  {{"path", temp.filePath("x.png")}, {"format", "png"}, {"width", 10}}))
              == "bad_param");
    }

    SECTION("PDF keeps the plot settings and the modified flag") {
        const double scaleBefore = graphic->getVariableDouble("$PSVPSCALE", 1.0);
        const RS_Vector baseBefore = graphic->getPaperInsertionBase();
        for (const bool modified : {false, true}) {
            CAPTURE(modified);
            graphic->setModified(modified);
            const QString pdf = temp.filePath(modified ? "modified.pdf" : "clean.pdf");
            const LC_ApiResponse exported = api.post("/api/v1/documents/export" + docQuery(window),
                                                     {{"path", pdf}, {"format", "pdf"}, {"fit_to_page", true}});
            REQUIRE(exported.status == 200);
            CHECK(fileStart(pdf, 4) == "%PDF");
            CHECK(graphic->getVariableDouble("$PSVPSCALE", 1.0) == scaleBefore);
            CHECK(graphic->getPaperInsertionBase() == baseBefore);
            CHECK(graphic->isModified() == modified);
        }
    }
    CHECK(api.modals.isEmpty());
}

// ================================================================= render and view

TEST_CASE("Remote API render: a PNG of the drawing, the user's view untouched", "[remote-api][documents][app]") {
    Api api;
    QC_MDIWindow* window = api.newDocument();
    addLine(window, {0, 0}, {100, 50});
    window->getGraphic()->setModified(false);
    LC_GraphicViewport* viewport = window->getGraphicView()->getViewPort();
    const int offsetX = viewport->getOffsetX();
    const int offsetY = viewport->getOffsetY();
    const double factor = viewport->getFactor().x;

    const LC_ApiResponse picture = api.get("/api/v1/render" + docQuery(window) + "&width=320&height=200");
    REQUIRE(picture.status == 200);
    CHECK(picture.contentType == "image/png");
    CHECK(picture.raw.startsWith(PngSignature));
    const QImage image = QImage::fromData(picture.raw, "PNG");
    CHECK(image.size() == QSize(320, 200));
    const QList<QByteArray> corners = header(picture, "X-LibreCAD-Window").split(',');
    REQUIRE(corners.size() == 4);
    const double minX = corners[0].toDouble();
    const double minY = corners[1].toDouble();
    const double maxX = corners[2].toDouble();
    const double maxY = corners[3].toDouble();
    CHECK(minX <= 0.0);
    CHECK(minY <= 0.0);
    CHECK(maxX >= 100.0);
    CHECK(maxY >= 50.0);
    const double scale = header(picture, "X-LibreCAD-Scale").toDouble();
    CHECK(scale > 0.0);
    CHECK(std::abs((maxX - minX) * scale - 320.0) < 1.0);
    CHECK(header(picture, "X-LibreCAD-Empty").isEmpty());

    CHECK(viewport->getOffsetX() == offsetX);
    CHECK(viewport->getOffsetY() == offsetY);
    CHECK(viewport->getFactor().x == factor);
    CHECK_FALSE(window->isModified());

    const LC_ApiResponse zoomed = api.get("/api/v1/render" + docQuery(window) + "&mode=window&min=0,0&max=10,10");
    REQUIRE(zoomed.status == 200);
    const QList<QByteArray> area = header(zoomed, "X-LibreCAD-Window").split(',');
    REQUIRE(area.size() == 4);
    CHECK(area[0].toDouble() <= 0.0);
    CHECK(area[2].toDouble() >= 10.0);
    CHECK(area[2].toDouble() < 100.0);

    CHECK(api.get("/api/v1/render" + docQuery(window) + "&mode=view").status == 200);
    CHECK(errorToken(api.get("/api/v1/render" + docQuery(window) + "&mode=window")) == "missing_param");
    CHECK(errorToken(api.get("/api/v1/render" + docQuery(window) + "&width=10")) == "bad_param");
    CHECK(errorToken(api.get("/api/v1/render" + docQuery(window) + "&mode=sideways")) == "bad_param");
    CHECK(errorToken(api.get("/api/v1/render" + docQuery(window) + "&bogus=1")) == "unknown_field");

    QC_MDIWindow* empty = api.newDocument();
    const LC_ApiResponse blank = api.get("/api/v1/render" + docQuery(empty));
    REQUIRE(blank.status == 200);
    CHECK(blank.raw.startsWith(PngSignature));
    CHECK(header(blank, "X-LibreCAD-Empty") == "1");

    // the renderer on its own
    QRectF rendered;
    double pixels = 0.0;
    const QByteArray direct = LC_RemoteApiRenderer::renderPng(window->getGraphic(), {QSize(128, 64), 0, false, true},
                                                              QRectF(), &rendered, &pixels);
    CHECK(direct.startsWith(PngSignature));
    CHECK(pixels > 0.0);
    CHECK(api.modals.isEmpty());
}

TEST_CASE("Remote API view: read, zoom to a window, zoom out and pan", "[remote-api][documents][app]") {
    Api api;
    QC_MDIWindow* window = api.newDocument();
    addLine(window, {0, 0}, {100, 100});

    const LC_ApiResponse view = api.get("/api/v1/view" + docQuery(window));
    REQUIRE(view.status == 200);
    CHECK(view.json.value("size_px").toArray().size() == 2);
    CHECK(view.json.value("px_per_unit").toDouble() > 0.0);
    CHECK(view.json.value("window").toObject().value("min").toArray().size() == 2);

    const LC_ApiResponse zoomed = api.post("/api/v1/view" + docQuery(window),
                                           {{"action", "window"}, {"min", QJsonArray{0, 0}}, {"max", QJsonArray{10, 10}}});
    REQUIRE(zoomed.status == 200);
    const QJsonObject area = zoomed.json.value("window").toObject();
    CHECK(area.value("min").toArray().at(0).toDouble() <= 0.01);
    CHECK(area.value("min").toArray().at(1).toDouble() <= 0.01);
    CHECK(area.value("max").toArray().at(0).toDouble() >= 9.99);
    CHECK(area.value("max").toArray().at(1).toDouble() >= 9.99);
    const double zoomedScale = zoomed.json.value("px_per_unit").toDouble();

    const LC_ApiResponse out = api.post("/api/v1/view" + docQuery(window), {{"action", "zoom_out"}, {"factor", 2}});
    REQUIRE(out.status == 200);
    CHECK(std::abs(out.json.value("px_per_unit").toDouble() * 2.0 - zoomedScale) < zoomedScale * 0.01);

    const double centerX = out.json.value("center").toArray().at(0).toDouble();
    const double scale = out.json.value("px_per_unit").toDouble();
    const LC_ApiResponse panned = api.post("/api/v1/view" + docQuery(window),
                                           {{"action", "pan"}, {"offset", QJsonArray{5, 0}}});
    REQUIRE(panned.status == 200);
    CHECK(std::abs(panned.json.value("center").toArray().at(0).toDouble() - (centerX + 5.0)) < 2.0 / scale);

    CHECK(api.post("/api/v1/view" + docQuery(window), {{"action", "extents"}}).status == 200);
    CHECK(errorToken(api.post("/api/v1/view" + docQuery(window), {{"action", "named"}, {"name", "nope"}}))
          == "unknown_view");
    CHECK(errorToken(api.post("/api/v1/view" + docQuery(window), {{"action", "window"}, {"factor", 2}}))
          == "bad_param");
    CHECK(errorToken(api.post("/api/v1/view" + docQuery(window), {})) == "missing_param");
    CHECK(api.modals.isEmpty());
}

// ================================================================= undo, command, drawing

TEST_CASE("Remote API undo and redo count steps", "[remote-api][documents][app]") {
    Api api;
    QC_MDIWindow* window = api.newDocument();
    for (int i = 0; i < 3; ++i) {
        addLine(window, {0.0, 10.0 * i}, {100.0, 10.0 * i});
    }
    REQUIRE(liveEntities(window) == 3);

    const LC_ApiResponse undone = api.post("/api/v1/undo" + docQuery(window), {{"steps", 2}});
    REQUIRE(undone.status == 200);
    CHECK(undone.json.value("undone").toInt() == 2);
    CHECK(undone.json.value("can_undo").toBool());
    CHECK(undone.json.value("can_redo").toBool());
    CHECK(undone.json.value("layers_and_blocks_not_undone").toBool());
    CHECK(liveEntities(window) == 1);

    const LC_ApiResponse redone = api.post("/api/v1/redo" + docQuery(window));
    REQUIRE(redone.status == 200);
    CHECK(redone.json.value("redone").toInt() == 1);
    CHECK(liveEntities(window) == 2);

    const LC_ApiResponse rest = api.post("/api/v1/undo" + docQuery(window), {{"steps", 5}});
    REQUIRE(rest.status == 200);
    CHECK(rest.json.value("undone").toInt() == 2);
    CHECK_FALSE(rest.json.value("can_undo").toBool());
    CHECK_FALSE(rest.json.value("warnings").toArray().isEmpty());
    CHECK(liveEntities(window) == 0);

    CHECK(errorToken(api.post("/api/v1/undo" + docQuery(window), {{"steps", 0}})) == "bad_param");
    CHECK(api.modals.isEmpty());
}

TEST_CASE("Remote API command line: typed commands draw", "[remote-api][documents][app]") {
    Api api;
    QC_MDIWindow* window = api.newDocument();
    REQUIRE(liveEntities(window) == 0);

    const LC_ApiResponse line = api.post("/api/v1/command" + docQuery(window),
                                         {{"command", "line"}, {"inputs", QJsonArray{"0,0", "100,0", "escape"}}});
    REQUIRE(line.status == 200);
    CHECK(line.json.value("executed").toArray().size() == 4);
    CHECK_FALSE(line.json.value("output").toArray().isEmpty());
    CHECK(line.json.value("unknown_commands").toArray().isEmpty());
    CHECK(liveEntities(window) == 1);
    api.post("/api/v1/command" + docQuery(window), {{"command", "escape"}});

    const LC_ApiResponse unknown = api.post("/api/v1/command" + docQuery(window), {{"command", "nosuchcommandxyz"}});
    REQUIRE(unknown.status == 200);
    CHECK(unknown.json.value("unknown_commands").toArray().contains(QJsonValue("nosuchcommandxyz")));

    CHECK(errorToken(api.post("/api/v1/command" + docQuery(window), {{"command", "   "}})) == "bad_param");
    CHECK(errorToken(api.post("/api/v1/command" + docQuery(window), {{"command", "line"}, {"inputs", QJsonArray{""}}}))
          == "bad_param");
    CHECK(errorToken(api.post("/api/v1/command" + docQuery(window), {{"command", "line"}, {"inputs", "0,0"}}))
          == "bad_param");

    LC_SET_ONE("RemoteAPI", "AllowCommands", false);
    const LC_ApiResponse disabled = api.post("/api/v1/command" + docQuery(window), {{"command", "line"}});
    LC_SET_ONE("RemoteAPI", "AllowCommands", true);
    CHECK(disabled.status == 403);
    CHECK(errorToken(disabled) == "commands_disabled");

    const LC_ApiResponse commands = api.get("/api/v1/commands");
    REQUIRE(commands.status == 200);
    // the main command names: "line" is an alias of "line2p"
    CHECK(commands.json.value("commands").toArray().contains(QJsonValue("line2p")));
    CHECK(api.modals.isEmpty());
}

TEST_CASE("Remote API drawing info and formats routes", "[remote-api][documents][app]") {
    Api api;
    QC_MDIWindow* window = api.newDocument();
    addLine(window, {0, 0}, {100, 50});

    const LC_ApiResponse info = api.get("/api/v1/drawing" + docQuery(window));
    REQUIRE(info.status == 200);
    CHECK_FALSE(info.json.value("units").toString().isEmpty());
    CHECK(info.json.value("unit_code").isDouble());
    CHECK(info.json.value("counts").toObject().value("total").toInt() == 1);
    CHECK(info.json.value("counts").toObject().value("by_type").toObject().value("line").toInt() == 1);
    const QJsonObject extents = info.json.value("extents").toObject();
    CHECK(extents.value("min").toArray().at(0).toDouble() == 0.0);
    CHECK(extents.value("max").toArray().at(0).toDouble() == 100.0);
    CHECK(extents.value("max").toArray().at(1).toDouble() == 50.0);
    CHECK(info.json.value("extents_all").toObject().value("max").toArray().at(1).toDouble() == 50.0);
    CHECK(info.json.value("undo").toObject().value("can_undo").toBool());
    CHECK_FALSE(info.json.value("undo").toObject().value("can_redo").toBool());
    CHECK(info.json.value("layers").toInt() >= 1);
    CHECK(info.json.value("current_layer").toString() == "0");
    CHECK(info.json.value("paper_space").toString() == "not_supported");
    CHECK(info.json.value("format").toObject().value("format").toString() == "dxf");
    CHECK(info.json.value("variables").toObject().contains("$INSUNITS"));

    const LC_ApiResponse formats = api.get("/api/v1/formats");
    REQUIRE(formats.status == 200);
    CHECK(formats.json.value("open").toArray().contains(QJsonValue(".dxf")));
    CHECK(formats.json.value("save").toArray().size() == 2);

    // unknown query parameters are refused on every route, not ignored (doc is always accepted)
    for (const QString& target : {QStringLiteral("/api/v1/drawing?detial=full"), QStringLiteral("/api/v1/formats?bogus=1"),
                                  QStringLiteral("/api/v1/documents?docs=1"), QStringLiteral("/api/v1/view?bogus=1"),
                                  QStringLiteral("/api/v1/commands?bogus=1")}) {
        CAPTURE(target);
        const LC_ApiResponse refused = api.get(target);
        CHECK(refused.status == 400);
        CHECK(errorToken(refused) == "unknown_field");
    }
    CHECK(api.post(QStringLiteral("/api/v1/undo?bogus=1") + QStringLiteral("&doc=%1").arg(window->getId())).status == 400);
    CHECK(api.get("/api/v1/formats?doc=0").status == 200);
    // a doc given twice would act on one of them at random
    const LC_ApiResponse twice = api.get(QStringLiteral("/api/v1/drawing?doc=%1&doc=%1").arg(window->getId()));
    CHECK(twice.status == 400);
    CHECK(twice.json.value("param").toString() == "doc");
    CHECK(api.modals.isEmpty());
}
