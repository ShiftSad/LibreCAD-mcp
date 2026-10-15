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

// lcRegisterEntityRoutes(): the entities, selection, measure, layers and
// blocks routes of the Remote API (design section 2.4, owner B).

#include <QJsonObject>

#include "lc_remoteapicontext.h"
#include "lc_remoteapientityindex.h"
#include "lc_remoteapientityjson.h"
#include "lc_remoteapientityops.h"
#include "lc_remoteapientityroutes.h"
#include "lc_remoteapilayerops.h"
#include "lc_remoteapirouter.h"
#include "lc_remoteapiroutes.h"

namespace {

namespace json = LC_RemoteApiEntityJson;

LC_RemoteApiEntityOps entityOps(LC_ApiCall& call) {
    const LC_ResolvedDoc& doc = call.doc();
    return LC_RemoteApiEntityOps(doc.document, doc.viewport);
}

LC_RemoteApiLayerOps layerOps(LC_ApiCall& call) {
    const LC_ResolvedDoc& doc = call.doc();
    return LC_RemoteApiLayerOps(doc.document, doc.viewport);
}

/** Only ?doc= is allowed. */
void noQuery(const LC_ApiCall& call) {
    LC_RemoteApiQuery::allowOnly(call.request.query, {});
}

QString param(const LC_ApiCall& call, const char* name) {
    return call.params.value(QLatin1String(name));
}

unsigned long long idParam(const LC_ApiCall& call) {
    return json::parseIdText(param(call, "id"), QStringLiteral("id"));
}

LC_RouteOptions options(const char* capability, const char* summary, const bool changesDocument) {
    LC_RouteOptions o;
    o.capability = QLatin1String(capability);
    o.summary = QLatin1String(summary);
    o.gui = true;
    o.changesDocument = changesDocument;
    o.needsDocument = true;
    return o;
}

} // namespace

namespace LC_RemoteApiEntityRoutes {

LC_ApiResponse listLayers(LC_ApiCall& call) {
    noQuery(call);
    return LC_ApiResponse::ok(layerOps(call).list());
}

LC_ApiResponse createLayer(LC_ApiCall& call) {
    noQuery(call);
    return LC_ApiResponse::ok(layerOps(call).create(call.body(), call.audit), 201);
}

LC_ApiResponse updateLayer(LC_ApiCall& call) {
    noQuery(call);
    return LC_ApiResponse::ok(layerOps(call).update(param(call, "name"), call.body(), call.warnings, call.audit));
}

LC_ApiResponse setCurrentLayer(LC_ApiCall& call) {
    noQuery(call);
    return LC_ApiResponse::ok(layerOps(call).setCurrent(param(call, "name")));
}

LC_ApiResponse deleteLayer(LC_ApiCall& call) {
    QJsonObject result = layerOps(call).remove(param(call, "name"), call.request.query, call.audit);
    // deleting a layer cannot be undone: always leave a line in the command history
    call.forceAudit = true;
    return LC_ApiResponse::ok(result);
}

LC_ApiResponse listBlocks(LC_ApiCall& call) {
    noQuery(call);
    return LC_ApiResponse::ok(layerOps(call).listBlocks());
}

LC_ApiResponse insertBlock(LC_ApiCall& call) {
    noQuery(call);
    return LC_ApiResponse::ok(layerOps(call).insertBlock(param(call, "name"), call.body(), call.warnings, call.audit), 201);
}

LC_ApiResponse listEntities(LC_ApiCall& call) {
    return LC_ApiResponse::ok(entityOps(call).list(call.request.query));
}

LC_ApiResponse getEntity(LC_ApiCall& call) {
    noQuery(call);
    return LC_ApiResponse::ok(entityOps(call).get(idParam(call)));
}

LC_ApiResponse createEntities(LC_ApiCall& call) {
    noQuery(call);
    return LC_ApiResponse::ok(entityOps(call).create(call.body(), call.warnings, call.audit), 201);
}

LC_ApiResponse updateEntity(LC_ApiCall& call) {
    noQuery(call);
    const unsigned long long id = idParam(call);
    return LC_ApiResponse::ok(entityOps(call).update(id, call.body(), call.warnings, call.audit));
}

LC_ApiResponse deleteEntity(LC_ApiCall& call) {
    noQuery(call);
    const unsigned long long id = idParam(call);
    return LC_ApiResponse::ok(entityOps(call).remove({id}, call.audit));
}

LC_ApiResponse deleteEntities(LC_ApiCall& call) {
    noQuery(call);
    const LC_JsonReader& body = call.body();
    body.allowOnly({"ids"});
    if (!body.has("ids")) {
        json::throwMissingParam(QStringLiteral("ids"));
    }
    const QList<unsigned long long> ids =
        json::parseIds(body.raw().value(QStringLiteral("ids")), QStringLiteral("ids"), LC_RemoteApiEntityOps::MaxBatch);
    return LC_ApiResponse::ok(entityOps(call).remove(ids, call.audit));
}

LC_ApiResponse transformEntities(LC_ApiCall& call) {
    noQuery(call);
    return LC_ApiResponse::ok(entityOps(call).transform(call.body(), call.audit));
}

LC_ApiResponse getSelection(LC_ApiCall& call) {
    noQuery(call);
    return LC_ApiResponse::ok(entityOps(call).selection());
}

LC_ApiResponse setSelection(LC_ApiCall& call) {
    noQuery(call);
    return LC_ApiResponse::ok(entityOps(call).select(call.body()));
}

LC_ApiResponse measure(LC_ApiCall& call) {
    noQuery(call);
    return LC_ApiResponse::ok(entityOps(call).measure(call.body()));
}

} // namespace LC_RemoteApiEntityRoutes

void lcRegisterEntityRoutes(LC_RemoteApiRouter& router) {
    using namespace LC_RemoteApiEntityRoutes;
    // literal paths before the {param} ones of the same prefix
    router.add("GET", QStringLiteral("/api/v1/layers"), options("layers", "List layers with color, linetype, lineweight, state and entity count", false),
               listLayers);
    router.add("POST", QStringLiteral("/api/v1/layers"), options("layers", "Create a layer (not undoable)", true), createLayer);
    router.add("POST", QStringLiteral("/api/v1/layers/{name}/current"), options("layers", "Make a layer the current layer", false),
               setCurrentLayer);
    router.add("PATCH", QStringLiteral("/api/v1/layers/{name}"),
               options("layers", "Rename, restyle, freeze or lock a layer (not undoable)", true), updateLayer);
    router.add("DELETE", QStringLiteral("/api/v1/layers/{name}"),
               options("layers", "Delete a layer; entities=refuse|delete|move (not undoable)", true), deleteLayer);

    router.add("GET", QStringLiteral("/api/v1/blocks"), options("blocks", "List block definitions", false), listBlocks);
    router.add("POST", QStringLiteral("/api/v1/blocks/{name}/insert"), options("blocks", "Insert a block reference (undoable)", true),
               insertBlock);

    router.add("GET", QStringLiteral("/api/v1/entities"),
               options("entities", "Query entities by type, layer, area and selection, with paging", false), listEntities);
    router.add("POST", QStringLiteral("/api/v1/entities"), options("entities", "Create entities in one undo step (atomic)", true),
               createEntities);
    router.add("POST", QStringLiteral("/api/v1/entities/delete"), options("entities", "Delete entities (undoable)", true), deleteEntities);
    router.add("POST", QStringLiteral("/api/v1/entities/transform"),
               options("transform", "Move, rotate, scale or mirror entities, optionally as copies (undoable)", true), transformEntities);
    router.add("GET", QStringLiteral("/api/v1/entities/{id}"), options("entities", "One entity in full detail", false), getEntity);
    router.add("PATCH", QStringLiteral("/api/v1/entities/{id}"),
               options("entities", "Change attributes or geometry of an entity; the id is kept (undoable)", true), updateEntity);
    router.add("DELETE", QStringLiteral("/api/v1/entities/{id}"), options("entities", "Delete an entity (undoable)", true), deleteEntity);

    router.add("GET", QStringLiteral("/api/v1/selection"), options("selection", "The ids of the selected entities", false), getSelection);
    router.add("POST", QStringLiteral("/api/v1/selection"), options("selection", "Change the selection: set, add, remove, clear, all", false),
               setSelection);
    router.add("POST", QStringLiteral("/api/v1/measure"),
               options("measure", "Measure distances, lengths, areas, angles and intersections (read-only)", false), measure);
}
