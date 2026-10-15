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

// TEST-ONLY stand-ins for the core component of the Remote API.
//
// The documents tests are written while the core implementation (router,
// JSON reader, context, GUI runner) is written in parallel. Until it exists,
// documents.cmake links this file into librecad_remote_api_documents_tests in
// its place. It follows the pinned headers and the design (sections 1.2, 2.1,
// 2.2, 3.2, 3.3) closely enough for the documents tests, nothing more: no
// modal guard, no capability or timeout policy. It is never part of
// librecad_lib, and never linked with the real core sources.

#include <atomic>
#include <chrono>
#include <future>
#include <memory>

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaObject>
#include <QTimer>
#include <QWidget>

#include "lc_remoteapicontext.h"
#include "lc_remoteapigui.h"
#include "lc_remoteapijson.h"
#include "lc_remoteapirouter.h"
#include "qc_applicationwindow.h"
#include "qc_mdiwindow.h"
#include "qg_graphicview.h"
#include "rs_settings.h"

namespace {

[[noreturn]] void fail(const int status, const char* token, const QString& detail, const char* key,
                       const QString& value) {
    throw LC_ApiError(status, QString::fromLatin1(token), detail, {{QString::fromLatin1(key), value}});
}

} // namespace

// ------------------------------------------------------------ LC_JsonReader

LC_JsonReader::LC_JsonReader(QJsonObject o, QString path)
    : m_object(std::move(o)), m_path(std::move(path)) {
}

namespace {
QString fieldPath(const QString& base, const char* key) {
    return base.isEmpty() ? QString::fromLatin1(key) : base + QLatin1Char('.') + QString::fromLatin1(key);
}
} // namespace

void LC_JsonReader::allowOnly(std::initializer_list<const char*> keys) const {
    for (auto it = m_object.begin(); it != m_object.end(); ++it) {
        bool known = false;
        for (const char* key : keys) {
            known = known || it.key() == QLatin1String(key);
        }
        if (!known) {
            const QString field = m_path.isEmpty() ? it.key() : m_path + QLatin1Char('.') + it.key();
            fail(400, "unknown_field", QStringLiteral("unknown field '%1'").arg(field), "field", field);
        }
    }
}

bool LC_JsonReader::has(const char* key) const {
    return m_object.contains(QLatin1String(key)) && !m_object.value(QLatin1String(key)).isNull();
}

double LC_JsonReader::num(const char* key) const {
    if (!has(key)) {
        fail(400, "missing_param", QStringLiteral("'%1' is required").arg(fieldPath(m_path, key)), "param",
             fieldPath(m_path, key));
    }
    return num(key, 0.0);
}

double LC_JsonReader::num(const char* key, const double def) const {
    if (!has(key)) {
        return def;
    }
    const QJsonValue value = m_object.value(QLatin1String(key));
    if (!value.isDouble()) {
        fail(400, "bad_param", QStringLiteral("'%1' must be a number").arg(fieldPath(m_path, key)), "param",
             fieldPath(m_path, key));
    }
    return value.toDouble();
}

int LC_JsonReader::integer(const char* key, const int def, const int min, const int max) const {
    if (!has(key)) {
        return def;
    }
    const double value = num(key, def);
    if (value != static_cast<double>(static_cast<long long>(value)) || value < min || value > max) {
        fail(400, "bad_param",
             QStringLiteral("'%1' must be an integer from %2 to %3").arg(fieldPath(m_path, key)).arg(min).arg(max),
             "param", fieldPath(m_path, key));
    }
    return static_cast<int>(value);
}

bool LC_JsonReader::flag(const char* key, const bool def) const {
    if (!has(key)) {
        return def;
    }
    const QJsonValue value = m_object.value(QLatin1String(key));
    if (!value.isBool()) {
        fail(400, "bad_param", QStringLiteral("'%1' must be true or false").arg(fieldPath(m_path, key)), "param",
             fieldPath(m_path, key));
    }
    return value.toBool();
}

QString LC_JsonReader::str(const char* key) const {
    if (!has(key)) {
        fail(400, "missing_param", QStringLiteral("'%1' is required").arg(fieldPath(m_path, key)), "param",
             fieldPath(m_path, key));
    }
    return str(key, QString());
}

QString LC_JsonReader::str(const char* key, const QString& def) const {
    if (!has(key)) {
        return def;
    }
    const QJsonValue value = m_object.value(QLatin1String(key));
    if (!value.isString()) {
        fail(400, "bad_param", QStringLiteral("'%1' must be a string").arg(fieldPath(m_path, key)), "param",
             fieldPath(m_path, key));
    }
    return value.toString();
}

namespace {
RS_Vector toPoint(const QJsonValue& value, const QString& path) {
    const QJsonArray array = value.toArray();
    if (!value.isArray() || array.size() < 2 || array.size() > 3 || !array.at(0).isDouble() || !array.at(1).isDouble()) {
        fail(400, "bad_param", QStringLiteral("'%1' must be a point [x, y]").arg(path), "param", path);
    }
    return {array.at(0).toDouble(), array.at(1).toDouble()};
}
} // namespace

RS_Vector LC_JsonReader::point(const char* key) const {
    if (!has(key)) {
        fail(400, "missing_param", QStringLiteral("'%1' is required").arg(fieldPath(m_path, key)), "param",
             fieldPath(m_path, key));
    }
    return toPoint(m_object.value(QLatin1String(key)), fieldPath(m_path, key));
}

std::optional<RS_Vector> LC_JsonReader::optPoint(const char* key) const {
    if (!has(key)) {
        return std::nullopt;
    }
    return point(key);
}

QList<RS_Vector> LC_JsonReader::points(const char* key, const int minCount) const {
    const QString path = fieldPath(m_path, key);
    if (!has(key)) {
        fail(400, "missing_param", QStringLiteral("'%1' is required").arg(path), "param", path);
    }
    const QJsonValue value = m_object.value(QLatin1String(key));
    if (!value.isArray() || value.toArray().size() < minCount) {
        fail(400, "bad_param", QStringLiteral("'%1' must list at least %2 points").arg(path).arg(minCount), "param",
             path);
    }
    QList<RS_Vector> result;
    const QJsonArray array = value.toArray();
    for (qsizetype i = 0; i < array.size(); ++i) {
        result.append(toPoint(array.at(i), QStringLiteral("%1[%2]").arg(path).arg(i)));
    }
    return result;
}

LC_JsonReader LC_JsonReader::object(const char* key) const {
    const QString path = fieldPath(m_path, key);
    if (!has(key) || !m_object.value(QLatin1String(key)).isObject()) {
        fail(400, has(key) ? "bad_param" : "missing_param", QStringLiteral("'%1' must be an object").arg(path),
             "param", path);
    }
    return {m_object.value(QLatin1String(key)).toObject(), path};
}

QList<LC_JsonReader> LC_JsonReader::objects(const char* key) const {
    const QString path = fieldPath(m_path, key);
    if (!has(key) || !m_object.value(QLatin1String(key)).isArray()) {
        fail(400, has(key) ? "bad_param" : "missing_param", QStringLiteral("'%1' must be a list").arg(path),
             "param", path);
    }
    QList<LC_JsonReader> result;
    const QJsonArray array = m_object.value(QLatin1String(key)).toArray();
    for (qsizetype i = 0; i < array.size(); ++i) {
        const QString itemPath = QStringLiteral("%1[%2]").arg(path).arg(i);
        if (!array.at(i).isObject()) {
            fail(400, "bad_param", QStringLiteral("'%1' must be an object").arg(itemPath), "param", itemPath);
        }
        result.append(LC_JsonReader(array.at(i).toObject(), itemPath));
    }
    return result;
}

QString LC_JsonReader::enumOf(const char* key, const QStringList& allowed, const QString& def) const {
    if (!has(key)) {
        return def;
    }
    const QString path = fieldPath(m_path, key);
    const QJsonValue value = m_object.value(QLatin1String(key));
    if (!value.isString() || !allowed.contains(value.toString())) {
        QJsonArray list;
        for (const QString& item : allowed) {
            list.append(item);
        }
        throw LC_ApiError(400, QStringLiteral("bad_param"),
                          QStringLiteral("'%1' must be one of: %2").arg(path, allowed.join(QStringLiteral(", "))),
                          {{QStringLiteral("param"), path}, {QStringLiteral("allowed"), list}});
    }
    return value.toString();
}

QJsonObject LC_JsonReader::raw() const {
    return m_object;
}

QJsonValue lcJsonPoint(const RS_Vector& point) {
    if (!point.valid || !std::isfinite(point.x) || !std::isfinite(point.y)) {
        return QJsonValue::Null;
    }
    return QJsonArray{point.x, point.y};
}

QString lcEntityTypeName(const RS2::EntityType type) {
    switch (type) {
        case RS2::EntityLine:
            return QStringLiteral("line");
        case RS2::EntityCircle:
            return QStringLiteral("circle");
        case RS2::EntityArc:
            return QStringLiteral("arc");
        case RS2::EntityPolyline:
            return QStringLiteral("polyline");
        case RS2::EntityInsert:
            return QStringLiteral("insert");
        default:
            return QStringLiteral("other");
    }
}

RS2::EntityType lcEntityTypeFromName(const QString& name) {
    if (name == QLatin1String("line")) {
        return RS2::EntityLine;
    }
    return RS2::EntityUnknown;
}

// ------------------------------------------------------------ LC_ApiCall

const LC_JsonReader& LC_ApiCall::body() {
    if (!m_body.has_value()) {
        QJsonObject object;
        if (!request.body.trimmed().isEmpty()) {
            QJsonParseError error{};
            const QJsonDocument document = QJsonDocument::fromJson(request.body, &error);
            if (error.error != QJsonParseError::NoError) {
                throw LC_ApiError(400, QStringLiteral("invalid_json"), error.errorString());
            }
            if (!document.isObject()) {
                throw LC_ApiError(400, QStringLiteral("body_must_be_object"),
                                  QStringLiteral("the request body must be a JSON object"));
            }
            object = document.object();
        }
        m_body.emplace(object);
    }
    return *m_body;
}

LC_ResolvedDoc& LC_ApiCall::doc() {
    if (!m_doc.has_value()) {
        m_doc = ctx.resolve(request);
    }
    return *m_doc;
}

// ------------------------------------------------------------ LC_RemoteApiGui

namespace {
int s_depth = 0;

struct Task {
    std::function<LC_ApiResponse()> fn;
    std::promise<LC_ApiResponse> promise;
    std::atomic<bool> cancelled{false};
};

void execute(const std::shared_ptr<Task>& task) {
    if (task->cancelled) {
        return;
    }
    if (s_depth > 0) {
        QTimer::singleShot(25, qApp, [task] { execute(task); });
        return;
    }
    if (QWidget* modal = QApplication::activeModalWidget()) {
        task->promise.set_value(LC_ApiError(409, QStringLiteral("gui_modal_open"), {},
                                            {{QStringLiteral("window"), modal->windowTitle()}})
                                    .toResponse());
        return;
    }
    ++s_depth;
    LC_ApiResponse response;
    try {
        response = task->fn();
    }
    catch (const LC_ApiError& error) {
        response = error.toResponse();
    }
    catch (const std::exception& error) {
        response = LC_ApiError(500, QStringLiteral("internal"), QString::fromUtf8(error.what())).toResponse();
    }
    --s_depth;
    if (!task->cancelled) {
        task->promise.set_value(std::move(response));
    }
}
} // namespace

LC_ApiResponse LC_RemoteApiGui::run(std::function<LC_ApiResponse()> fn, const int timeoutMs, const QString& route) {
    auto task = std::make_shared<Task>();
    task->fn = std::move(fn);
    auto future = task->promise.get_future();
    QMetaObject::invokeMethod(qApp, [task] { execute(task); }, Qt::QueuedConnection);
    if (future.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
        task->cancelled = true;
        throw LC_ApiError(504, QStringLiteral("ui_timeout"), QStringLiteral("LibreCAD did not answer in time: %1").arg(route));
    }
    return future.get();
}

int LC_RemoteApiGui::depth() {
    return s_depth;
}

// ------------------------------------------------------------ LC_RemoteApiContext

LC_RemoteApiContext::LC_RemoteApiContext(QC_ApplicationWindow* appWindow)
    : m_appWindow(appWindow) {
}

QC_ApplicationWindow* LC_RemoteApiContext::appWindow() const {
    return m_appWindow;
}

LC_RemoteApiConfig LC_RemoteApiContext::config() const {
    LC_RemoteApiConfig config;
    config.enabled = LC_GET_ONE_BOOL("RemoteAPI", "Enabled", false);
    config.port = LC_GET_ONE_INT("RemoteAPI", "Port", LC_RemoteApiConfig::DefaultPort);
    config.token = LC_GET_ONE_STR("RemoteAPI", "Token", "");
    config.notify = LC_GET_ONE_BOOL("RemoteAPI", "Notify", true);
    config.allowCommands = LC_GET_ONE_BOOL("RemoteAPI", "AllowCommands", true);
    config.allowedRoots = LC_GET_ONE_STR("RemoteAPI", "AllowedRoots", "").split(QLatin1Char(';'), Qt::SkipEmptyParts);
    config.allowNetworkPaths = LC_GET_ONE_BOOL("RemoteAPI", "AllowNetworkPaths", false);
    return config;
}

QList<QC_MDIWindow*> LC_RemoteApiContext::documentWindows() const {
    QList<QC_MDIWindow*> windows;
    if (m_appWindow == nullptr) {
        return windows;
    }
    m_appWindow->doForEachWindow([&windows](QC_MDIWindow* window) {
        if (window != nullptr && window->getParentWindow() == nullptr && !window->getGraphicView()->isPrintPreview()) {
            windows.append(window);
        }
    });
    return windows;
}

QC_MDIWindow* LC_RemoteApiContext::activeDocumentWindow() const {
    if (m_appWindow == nullptr) {
        return nullptr;
    }
    QC_MDIWindow* window = m_appWindow->getCurrentMDIWindow();
    while (window != nullptr && window->getParentWindow() != nullptr) {
        window = window->getParentWindow();
    }
    return window;
}

LC_ResolvedDoc LC_RemoteApiContext::resolve(const LC_ApiRequest& request) const {
    const QList<QString> ids = request.query.values(QStringLiteral("doc"));
    if (!ids.isEmpty()) {
        bool ok = false;
        const unsigned id = ids.first().toUInt(&ok);
        if (!ok || ids.size() > 1) {
            fail(400, "bad_param", QStringLiteral("doc must be a document id"), "param", QStringLiteral("doc"));
        }
        for (QC_MDIWindow* window : documentWindows()) {
            if (window->getId() == id) {
                return resolve(window);
            }
        }
        throw LC_ApiError(404, QStringLiteral("unknown_document"), QStringLiteral("no open drawing has id %1").arg(id));
    }
    QC_MDIWindow* active = activeDocumentWindow();
    if (active == nullptr) {
        throw LC_ApiError(409, QStringLiteral("no_document"), QStringLiteral("no drawing is open"));
    }
    return resolve(active);
}

LC_ResolvedDoc LC_RemoteApiContext::resolve(QC_MDIWindow* window) const {
    LC_ResolvedDoc resolved;
    resolved.id = window->getId();
    resolved.window = window;
    resolved.document = window->getDocument();
    resolved.graphic = window->getGraphic();
    resolved.view = window->getGraphicView();
    resolved.viewport = resolved.view != nullptr ? resolved.view->getViewPort() : nullptr;
    return resolved;
}

QString LC_RemoteApiContext::cancelActiveAction(const LC_ResolvedDoc& doc) const {
    if (doc.view == nullptr) {
        return {};
    }
    const QString name = doc.view->getCurrentActionName();
    doc.view->killAllActions();
    return name;
}

void LC_RemoteApiContext::notify(const QString& line, const bool force) const {
    if (m_appWindow != nullptr && (force || config().notify)) {
        m_appWindow->commandMessage(line);
    }
}

// ------------------------------------------------------------ LC_RemoteApiRouter

void LC_RemoteApiRouter::add(const QByteArray& method, const QString& pattern, LC_RouteOptions options,
                             LC_ApiHandler handler) {
    m_routes.append({method, pattern, pattern.split(QLatin1Char('/')), std::move(options), std::move(handler)});
}

LC_ApiResponse LC_RemoteApiRouter::dispatch(const LC_ApiRequest& request, LC_RemoteApiContext& ctx) const {
    const QStringList segments = request.path.split(QLatin1Char('/'));
    QStringList allowed;
    for (const Route& route : m_routes) {
        if (route.segments.size() != segments.size()) {
            continue;
        }
        QHash<QString, QString> params;
        bool matches = true;
        for (qsizetype i = 0; i < segments.size() && matches; ++i) {
            const QString& part = route.segments.at(i);
            if (part.startsWith(QLatin1Char('{')) && part.endsWith(QLatin1Char('}'))) {
                params.insert(part.mid(1, part.size() - 2), segments.at(i));
            }
            else {
                matches = part == segments.at(i);
            }
        }
        if (!matches) {
            continue;
        }
        if (route.method != request.method) {
            allowed.append(QString::fromLatin1(route.method));
            continue;
        }
        const auto handle = [&route, &request, &ctx, params]() {
            LC_ApiCall call(request, ctx);
            call.params = params;
            if (route.options.needsDocument) {
                call.setDoc(ctx.resolve(request));
            }
            if (route.options.changesDocument && !route.options.keepActiveAction) {
                if (call.hasDoc() || ctx.activeDocumentWindow() != nullptr) {
                    call.cancelledAction = ctx.cancelActiveAction(call.hasDoc() ? call.doc() : ctx.resolve(request));
                }
            }
            LC_ApiResponse response = route.handler(call);
            if (response.raw.isEmpty()) {
                if (!call.warnings.isEmpty()) {
                    response.json.insert(QStringLiteral("warnings"), call.warnings);
                }
                if (!call.cancelledAction.isEmpty()) {
                    response.json.insert(QStringLiteral("cancelled_action"), call.cancelledAction);
                }
            }
            if (!call.audit.isEmpty()) {
                ctx.notify(call.audit, call.forceAudit);
            }
            return response;
        };
        try {
            return route.options.gui ? LC_RemoteApiGui::run(handle, route.options.timeoutMs, route.pattern) : handle();
        }
        catch (const LC_ApiError& error) {
            return error.toResponse();
        }
    }
    if (!allowed.isEmpty()) {
        QJsonArray methods;
        for (const QString& method : std::as_const(allowed)) {
            methods.append(method);
        }
        return LC_ApiError(405, QStringLiteral("method_not_allowed"), {}, {{QStringLiteral("allowed"), methods}})
            .toResponse();
    }
    return LC_ApiError(404, QStringLiteral("not_found")).toResponse();
}

QJsonArray LC_RemoteApiRouter::describe() const {
    QJsonArray routes;
    for (const Route& route : m_routes) {
        routes.append(QJsonObject{{QStringLiteral("method"), QString::fromLatin1(route.method)},
                                  {QStringLiteral("path"), route.pattern},
                                  {QStringLiteral("capability"), route.options.capability},
                                  {QStringLiteral("changes_document"), route.options.changesDocument},
                                  {QStringLiteral("timeout_s"), route.options.timeoutMs / 1000.0},
                                  {QStringLiteral("summary"), route.options.summary}});
    }
    return routes;
}

QStringList LC_RemoteApiRouter::capabilities() const {
    QStringList list;
    for (const Route& route : m_routes) {
        if (!list.contains(route.options.capability)) {
            list.append(route.options.capability);
        }
    }
    return list;
}
