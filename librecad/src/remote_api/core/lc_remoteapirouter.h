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

#ifndef LC_REMOTEAPIROUTER_H
#define LC_REMOTEAPIROUTER_H

// Route table of the Remote API and the per-request call object handed to handlers.
// Pinned interface (implemented in lc_remoteapirouter.cpp by the core component).

#include <functional>
#include <optional>

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

#include "lc_remoteapicontext.h"
#include "lc_remoteapijson.h"
#include "lc_remoteapitypes.h"

/**
 * Options of one route (see the route list, design section 2.4).
 */
struct LC_RouteOptions {
    QString capability; QString summary;   // capability: reported by /status and /routes; summary: one line for /routes
    bool gui = true;              // run handler via LC_RemoteApiGui::run (forced on by needsDocument/changesDocument)
    bool changesDocument = false; // cancel active action (unless keepActiveAction), audit line, refuse on user modal
    bool keepActiveAction = false;
    bool needsDocument = false;   // resolve ?doc before handler (409 no_document / 404 unknown_document)
    int timeoutMs = 10000;
};

/**
 * One request as seen by a handler (GUI thread for gui routes).
 */
class LC_ApiCall {
public:
    LC_ApiCall(const LC_ApiRequest& request, LC_RemoteApiContext& ctx);

    const LC_ApiRequest& request;
    QHash<QString, QString> params;            // {name} captures, decoded
    LC_RemoteApiContext& ctx;
    // The body parsed as a JSON object, once; an empty body reads as {}.
    const LC_JsonReader& body();               // parsed once; 400 invalid_json / body_must_be_object
    // The document of ?doc= (or the active one). The router resolves it before
    // the handler runs when the route has needsDocument; on other routes the
    // first call resolves it and throws as LC_RemoteApiContext::resolve does.
    LC_ResolvedDoc& doc();                     // valid when needsDocument
    QJsonArray warnings; QString cancelledAction; QString audit;  // audit: "[MCP] ..." line (set by handler)
    bool forceAudit = false;                   // write audit even with Notify off (discard on close, layer delete, overwrite)

    bool hasDoc() const;
    void setDoc(const LC_ResolvedDoc& resolved);   // used by the router, and by tests without a main window

private:
    std::optional<LC_JsonReader> m_body;
    std::optional<LC_ResolvedDoc> m_doc;
};

using LC_ApiHandler = std::function<LC_ApiResponse(LC_ApiCall&)>;

/**
 * Method and path matching for the Remote API.
 *
 * pattern is the full path including the version prefix, for example
 * "/api/v1/entities/{id}". Matching is exact per segment; each {name} captures
 * one percent-decoded segment into LC_ApiCall::params. An unknown path gives a
 * bare 404 not_found, a known path with another method 405 method_not_allowed.
 * Routes are added on the GUI thread before the server starts; dispatch() runs
 * on the server thread and posts gui routes to the GUI thread.
 */
class LC_RemoteApiRouter {
public:
    void add(const QByteArray& method, const QString& pattern, LC_RouteOptions options, LC_ApiHandler handler);
    LC_ApiResponse dispatch(const LC_ApiRequest& request, LC_RemoteApiContext& ctx) const;   // server thread
    QJsonArray describe() const; QStringList capabilities() const;

private:
    struct Route {
        QByteArray method;
        QString pattern;
        QStringList segments;                  // pattern split on '/'
        LC_RouteOptions options;
        LC_ApiHandler handler;
    };
    QList<Route> m_routes;
};

inline LC_ApiCall::LC_ApiCall(const LC_ApiRequest& apiRequest, LC_RemoteApiContext& context)
    : request(apiRequest)
    , ctx(context) {
}

inline bool LC_ApiCall::hasDoc() const {
    return m_doc.has_value();
}

inline void LC_ApiCall::setDoc(const LC_ResolvedDoc& resolved) {
    m_doc = resolved;
}

#endif
