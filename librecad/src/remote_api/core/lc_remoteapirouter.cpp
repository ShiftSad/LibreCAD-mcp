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

#include "lc_remoteapirouter.h"

#include <QJsonDocument>
#include <QJsonObject>

#include "lc_remoteapigui.h"
#include "lc_remoteapihttp.h"

namespace {

bool isParam(const QString& segment) {
    return segment.size() > 2 && segment.startsWith(QLatin1Char('{')) && segment.endsWith(QLatin1Char('}'));
}

// The decoded segments of a request path; a raw path keeps encoded slashes
// inside their segment.
QStringList requestSegments(const LC_ApiRequest& request) {
    if (request.rawPath.isEmpty()) {
        return request.path.split(QLatin1Char('/'));
    }
    QStringList segments = request.rawPath.split(QLatin1Char('/'));
    for (QString& segment : segments) {
        segment = LC_RemoteApiHttp::urlDecode(segment.toLatin1(), false);
    }
    return segments;
}

void appendWarnings(LC_ApiResponse& response, const QJsonArray& warnings) {
    if (warnings.isEmpty() || !response.raw.isEmpty()) {
        return;
    }
    QJsonArray all = response.json.value(QStringLiteral("warnings")).toArray();
    for (const QJsonValue& warning : warnings) {
        all.append(warning);
    }
    response.json.insert(QStringLiteral("warnings"), all);
}

} // namespace

const LC_JsonReader& LC_ApiCall::body() {
    if (!m_body) {
        const QByteArray& text = request.body;
        if (text.trimmed().isEmpty()) {
            m_body.emplace(QJsonObject());
        }
        else {
            QJsonParseError error{};
            const QJsonDocument document = QJsonDocument::fromJson(text, &error);
            if (error.error != QJsonParseError::NoError) {
                throw LC_ApiError(400, QStringLiteral("invalid_json"),
                                  QStringLiteral("%1 at offset %2").arg(error.errorString()).arg(error.offset));
            }
            if (!document.isObject()) {
                throw LC_ApiError(400, QStringLiteral("body_must_be_object"),
                                  QStringLiteral("the request body must be a JSON object"));
            }
            m_body.emplace(document.object());
        }
    }
    return *m_body;
}

LC_ResolvedDoc& LC_ApiCall::doc() {
    if (!m_doc) {
        m_doc = ctx.resolve(request);
    }
    return *m_doc;
}

void LC_RemoteApiRouter::add(const QByteArray& method, const QString& pattern, LC_RouteOptions options,
                             LC_ApiHandler handler) {
    // A document lives on the GUI thread: a route that resolves or changes one
    // runs there whatever gui says.
    if (!options.gui && (options.needsDocument || options.changesDocument)) {
        qWarning("remote-api: %s %s needs the document, so it runs on the GUI thread", method.constData(),
                 qPrintable(pattern));
        options.gui = true;
    }
    Route route;
    route.method = method.toUpper();
    route.pattern = pattern;
    route.segments = pattern.split(QLatin1Char('/'));
    route.options = std::move(options);
    route.handler = std::move(handler);
    m_routes.append(std::move(route));
}

LC_ApiResponse LC_RemoteApiRouter::dispatch(const LC_ApiRequest& request, LC_RemoteApiContext& ctx) const {
    try {
        const QStringList segments = requestSegments(request);

        // Every route whose pattern matches the path; among those with the
        // request's method the most literal one wins (".../entities/delete"
        // over ".../entities/{id}"), then the first added.
        const Route* best = nullptr;
        int bestLiterals = -1;
        QHash<QString, QString> bestParams;
        QStringList allowed;
        for (const Route& route : m_routes) {
            if (route.segments.size() != segments.size()) {
                continue;
            }
            QHash<QString, QString> params;
            int literals = 0;
            bool matches = true;
            for (qsizetype i = 0; i < segments.size() && matches; ++i) {
                const QString& expected = route.segments.at(i);
                if (isParam(expected)) {
                    matches = !segments.at(i).isEmpty();
                    params.insert(expected.mid(1, expected.size() - 2), segments.at(i));
                }
                else {
                    matches = expected == segments.at(i);
                    ++literals;
                }
            }
            if (!matches) {
                continue;
            }
            const QString method = QString::fromLatin1(route.method);
            if (!allowed.contains(method)) {
                allowed << method;
            }
            if (route.method == request.method && literals > bestLiterals) {
                best = &route;
                bestLiterals = literals;
                bestParams = params;
            }
        }

        if (best == nullptr) {
            if (allowed.isEmpty()) {
                return LC_ApiError(404, QStringLiteral("not_found"),
                                   QStringLiteral("no route %1 %2").arg(QString::fromLatin1(request.method), request.path))
                    .toResponse();
            }
            allowed.sort();
            LC_ApiResponse response =
                LC_ApiError(405, QStringLiteral("method_not_allowed"),
                            QStringLiteral("%1 is not allowed on %2").arg(QString::fromLatin1(request.method), request.path),
                            {{QStringLiteral("allowed"), QJsonArray::fromStringList(allowed)}})
                    .toResponse();
            response.extraHeaders.append({QByteArrayLiteral("Allow"), allowed.join(QStringLiteral(", ")).toLatin1()});
            return response;
        }

        // Everything the task needs is captured by value: after a ui_timeout the
        // task may still run, when this request and its caller are gone.
        const LC_RouteOptions options = best->options;
        const LC_ApiHandler handler = best->handler;
        LC_RemoteApiContext* context = &ctx;
        auto task = [request, options, handler, context, params = bestParams]() -> LC_ApiResponse {
            LC_ApiCall call(request, *context);
            call.params = params;
            try {
                if (options.needsDocument) {
                    call.setDoc(context->resolve(request));
                }
                if (options.changesDocument && !options.keepActiveAction) {
                    if (call.hasDoc()) {
                        call.cancelledAction = context->cancelActiveAction(call.doc());
                    }
                    else {
                        // a route on no particular document (new, open): cancel
                        // the command of the document the user is working in
                        try {
                            call.cancelledAction = context->cancelActiveAction(context->resolve(request));
                        }
                        catch (const LC_ApiError&) {
                            // no document open, nothing to cancel
                        }
                    }
                }
                LC_ApiResponse response = handler(call);
                if (response.status < 400 && !call.audit.isEmpty()) {
                    if (options.gui) {
                        context->notify(call.audit, call.forceAudit);
                    }
                    else {
                        // the command line and the settings belong to the GUI thread;
                        // a stop abandons the task, so it never outlives the context
                        try {
                            LC_RemoteApiGui::run(
                                [context, line = call.audit, force = call.forceAudit] {
                                    context->notify(line, force);
                                    return LC_ApiResponse();
                                },
                                2000, QStringLiteral("audit"), LC_RemoteApiGui::ModalPolicy::Allow);
                        }
                        catch (const LC_ApiError&) {
                            // the line is lost; the response stands
                        }
                    }
                }
                appendWarnings(response, call.warnings);
                if (!call.cancelledAction.isEmpty() && response.raw.isEmpty()) {
                    response.json.insert(QStringLiteral("cancelled_action"), call.cancelledAction);
                }
                return response;
            }
            catch (const LC_ApiError& e) {
                LC_ApiResponse response = e.toResponse();
                if (!call.cancelledAction.isEmpty()) {
                    response.json.insert(QStringLiteral("cancelled_action"), call.cancelledAction);
                }
                return response;
            }
        };

        if (options.gui) {
            const QString route = QString::fromLatin1(best->method) + QLatin1Char(' ') + best->pattern;
            return LC_RemoteApiGui::run(std::move(task), options.timeoutMs, route);
        }
        return task();
    }
    catch (const LC_ApiError& e) {
        return e.toResponse();
    }
    catch (const std::exception& e) {
        return LC_ApiError(500, QStringLiteral("internal"), QString::fromLocal8Bit(e.what())).toResponse();
    }
    catch (...) {
        return LC_ApiError(500, QStringLiteral("internal"), QStringLiteral("unknown exception")).toResponse();
    }
}

QJsonArray LC_RemoteApiRouter::describe() const {
    QJsonArray routes;
    for (const Route& route : m_routes) {
        routes.append(QJsonObject{
            {QStringLiteral("method"), QString::fromLatin1(route.method)},
            {QStringLiteral("path"), route.pattern},
            {QStringLiteral("capability"), route.options.capability},
            {QStringLiteral("changes_document"), route.options.changesDocument},
            {QStringLiteral("timeout_s"), route.options.timeoutMs / 1000.0},
            {QStringLiteral("summary"), route.options.summary},
        });
    }
    return routes;
}

QStringList LC_RemoteApiRouter::capabilities() const {
    QStringList result;
    for (const Route& route : m_routes) {
        if (!route.options.capability.isEmpty() && !result.contains(route.options.capability)) {
            result << route.options.capability;
        }
    }
    return result;
}
