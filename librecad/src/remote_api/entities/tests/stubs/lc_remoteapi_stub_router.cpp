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

// TEST-ONLY stand-in for core/lc_remoteapirouter.cpp.
//
// entities.cmake adds it to the test sources only while the core component's
// router is not on the branch; it drops out by itself once the real one lands.
// It implements the parts the entities tests use (LC_ApiCall::body/doc, the
// route table and describe/capabilities) after the contract of
// core/lc_remoteapirouter.h. dispatch() runs a handler synchronously, without
// the GUI runner, and is not used by the tests.

#include <QJsonDocument>
#include <QJsonParseError>

#include "lc_remoteapirouter.h"

const LC_JsonReader& LC_ApiCall::body() {
    if (!m_body.has_value()) {
        const QByteArray text = request.body.trimmed();
        QJsonObject object;
        if (!text.isEmpty()) {
            QJsonParseError error{};
            const QJsonDocument document = QJsonDocument::fromJson(text, &error);
            if (error.error != QJsonParseError::NoError) {
                throw LC_ApiError(400, QStringLiteral("invalid_json"), error.errorString());
            }
            if (!document.isObject()) {
                throw LC_ApiError(400, QStringLiteral("body_must_be_object"), QStringLiteral("the request body must be a JSON object"));
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

void LC_RemoteApiRouter::add(const QByteArray& method, const QString& pattern, LC_RouteOptions options, LC_ApiHandler handler) {
    m_routes.append(Route{method, pattern, pattern.split(QLatin1Char('/')), std::move(options), std::move(handler)});
}

LC_ApiResponse LC_RemoteApiRouter::dispatch(const LC_ApiRequest& request, LC_RemoteApiContext& ctx) const {
    const QStringList segments = request.path.split(QLatin1Char('/'));
    for (const Route& route : m_routes) {
        if (route.method != request.method || route.segments.size() != segments.size()) {
            continue;
        }
        QHash<QString, QString> params;
        bool match = true;
        for (int i = 0; i < segments.size() && match; ++i) {
            const QString& part = route.segments.at(i);
            if (part.startsWith(QLatin1Char('{')) && part.endsWith(QLatin1Char('}'))) {
                params.insert(part.mid(1, part.size() - 2), segments.at(i));
            }
            else {
                match = part == segments.at(i);
            }
        }
        if (!match) {
            continue;
        }
        LC_ApiCall call(request, ctx);
        call.params = params;
        try {
            return route.handler(call);
        }
        catch (const LC_ApiError& e) {
            return e.toResponse();
        }
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
    QStringList out;
    for (const Route& route : m_routes) {
        if (!route.options.capability.isEmpty() && !out.contains(route.options.capability)) {
            out.append(route.options.capability);
        }
    }
    return out;
}
