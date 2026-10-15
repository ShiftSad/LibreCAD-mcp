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

// Layers and blocks of the Remote API (design section 5.1, "Layers" and
// "Blocks"), and the route table and handlers of the entities component.

#include <catch2/catch_test_macros.hpp>

#include <QSet>

#include "lc_remoteapi_entities_testsupport.h"
#include "lc_remoteapientityroutes.h"
#include "lc_remoteapiroutes.h"
#include "rs_block.h"
#include "rs_line.h"

using namespace lc::test::remoteapi;

namespace {

QJsonObject layerNamed(Drawing& d, const QString& name) {
    for (const QJsonValue& v : d.layers().list().value(QStringLiteral("layers")).toArray()) {
        if (v.toObject().value(QStringLiteral("name")).toString() == name) {
            return v.toObject();
        }
    }
    return {};
}

QMultiHash<QString, QString> query(std::initializer_list<std::pair<const char*, const char*>> pairs) {
    QMultiHash<QString, QString> out;
    for (const auto& [key, value] : pairs) {
        out.insert(QString::fromLatin1(key), QString::fromLatin1(value));
    }
    return out;
}

} // namespace

TEST_CASE("Remote API layers: create, rename, freeze, lock and make current", "[remote-api][layers]") {
    Drawing d;
    QString audit;
    QJsonArray warnings;

    const QJsonObject created = d.layers().create(
        LC_JsonReader(object(R"({"name":"walls","color":"#ff0000","linetype":"Dashed","lineweight":0.5,"make_current":true})")), audit);
    CHECK_FALSE(created.value(QStringLiteral("undoable")).toBool(true));
    const QJsonObject walls = created.value(QStringLiteral("layer")).toObject();
    CHECK(walls.value(QStringLiteral("name")).toString() == QStringLiteral("walls"));
    CHECK(walls.value(QStringLiteral("color")).toString() == QStringLiteral("#ff0000"));
    CHECK(walls.value(QStringLiteral("aci")).toInt() == 1);
    CHECK(walls.value(QStringLiteral("linetype")).toString() == QStringLiteral("DASHED"));
    CHECK(walls.value(QStringLiteral("lineweight")).toDouble() == 0.5);
    CHECK(walls.value(QStringLiteral("current")).toBool());
    CHECK(walls.value(QStringLiteral("printable")).toBool());
    CHECK(audit == QStringLiteral("[MCP] created layer 'walls'"));
    const QJsonObject listed = d.layers().list();
    CHECK(listed.value(QStringLiteral("current")).toString() == QStringLiteral("walls"));
    CHECK(listed.value(QStringLiteral("layers")).toArray().size() == 2);

    // a duplicate name is refused, and the layer is not merged into
    const LC_ApiError duplicate = errorOf([&] { d.layers().create(LC_JsonReader(object(R"({"name":"walls","color":"#00ff00"})")), audit); });
    CHECK(duplicate.status == 409);
    CHECK(duplicate.token == QStringLiteral("layer_exists"));
    CHECK(layerNamed(d, QStringLiteral("walls")).value(QStringLiteral("color")).toString() == QStringLiteral("#ff0000"));
    CHECK(errorOf([&] { d.layers().create(LC_JsonReader(object(R"({"name":"a/b"})")), audit); }).token == QStringLiteral("bad_param"));
    CHECK(errorOf([&] { d.layers().create(LC_JsonReader(object(R"({"name":""})")), audit); }).token == QStringLiteral("bad_param"));
    CHECK(errorOf([&] { d.layers().create(LC_JsonReader(object(R"({"name":"x","color":"ByLayer"})")), audit); }).token ==
          QStringLiteral("bad_param"));
    CHECK(errorOf([&] { d.layers().create(LC_JsonReader(object(R"({"name":"x","colour":"red"})")), audit); }).token ==
          QStringLiteral("unknown_field"));
    CHECK(errorOf([&] { d.layers().create(LC_JsonReader(object(R"({"name":"x","frozen":true,"make_current":true})")), audit); }).token ==
          QStringLiteral("layer_frozen"));
    CHECK(d.graphic.findLayer(QStringLiteral("x")) == nullptr);

    // entities on the layer follow a rename
    const unsigned long long line = d.createOne(R"({"entities":[{"type":"line","start":[0,0],"end":[1,0]}]})");
    QJsonObject renamed = d.layers().update(QStringLiteral("walls"), LC_JsonReader(object(R"({"new_name":"walls2"})")), warnings, audit)
                              .value(QStringLiteral("layer")).toObject();
    CHECK(renamed.value(QStringLiteral("name")).toString() == QStringLiteral("walls2"));
    CHECK(renamed.value(QStringLiteral("entity_count")).toInt() == 1);
    CHECK(d.graphic.findLayer(QStringLiteral("walls")) == nullptr);
    CHECK(d.ops().get(line).value(QStringLiteral("entity")).toObject().value(QStringLiteral("layer")).toString() == QStringLiteral("walls2"));
    CHECK(audit == QStringLiteral("[MCP] renamed layer 'walls' to 'walls2'"));

    CHECK(errorOf([&] { d.layers().update(QStringLiteral("0"), LC_JsonReader(object(R"({"new_name":"zero"})")), warnings, audit); }).token ==
          QStringLiteral("cannot_rename_layer_0"));
    d.layers().create(LC_JsonReader(object(R"({"name":"doors"})")), audit);
    CHECK(errorOf([&] { d.layers().update(QStringLiteral("doors"), LC_JsonReader(object(R"({"new_name":"walls2"})")), warnings, audit); }).token ==
          QStringLiteral("layer_exists"));
    CHECK(errorOf([&] { d.layers().update(QStringLiteral("nope"), LC_JsonReader(object(R"({"locked":true})")), warnings, audit); }).token ==
          QStringLiteral("unknown_layer"));

    // freeze (hide) the current layer: the current layer moves away, with a warning
    warnings = {};
    QJsonObject frozen = d.layers().update(QStringLiteral("walls2"), LC_JsonReader(object(R"({"frozen":true})")), warnings, audit)
                             .value(QStringLiteral("layer")).toObject();
    CHECK(frozen.value(QStringLiteral("frozen")).toBool());
    CHECK_FALSE(frozen.value(QStringLiteral("current")).toBool());
    CHECK(warnings.size() == 1);
    CHECK(d.ops().list({}).value(QStringLiteral("total")).toInt() == 0);
    const LC_ApiError current = errorOf([&] { d.layers().setCurrent(QStringLiteral("walls2")); });
    CHECK(current.status == 409);
    CHECK(current.token == QStringLiteral("layer_frozen"));

    d.layers().update(QStringLiteral("walls2"), LC_JsonReader(object(R"({"frozen":false,"locked":true,"color":3,"printable":false})")), warnings, audit);
    const QJsonObject locked = layerNamed(d, QStringLiteral("walls2"));
    CHECK_FALSE(locked.value(QStringLiteral("frozen")).toBool());
    CHECK(locked.value(QStringLiteral("locked")).toBool());
    CHECK_FALSE(locked.value(QStringLiteral("printable")).toBool());
    CHECK(locked.value(QStringLiteral("aci")).toInt() == 3);
    CHECK(d.ops().get(line).value(QStringLiteral("entity")).toObject().value(QStringLiteral("locked")).toBool());

    CHECK(d.layers().setCurrent(QStringLiteral("walls2")).value(QStringLiteral("current")).toString() == QStringLiteral("walls2"));
    CHECK(d.graphic.getActiveLayer()->getName() == QStringLiteral("walls2"));
}

TEST_CASE("Remote API layers: delete refuses, deletes or moves the entities", "[remote-api][layers]") {
    Drawing d;
    QString audit;
    d.addLayer(QStringLiteral("doors"));
    const unsigned long long a = d.createOne(R"({"entities":[{"type":"line","start":[0,0],"end":[1,0],"layer":"doors"}]})");
    const unsigned long long b = d.createOne(R"({"entities":[{"type":"circle","center":[0,0],"radius":1,"layer":"doors"}]})");
    // an edit leaves an earlier version of the line in the undo history, on the same layer
    QJsonArray warnings;
    d.ops().update(a, LC_JsonReader(object(R"({"end":[2,0]})")), warnings, audit);

    CHECK(errorOf([&] { d.layers().remove(QStringLiteral("0"), {}, audit); }).token == QStringLiteral("cannot_delete_layer_0"));
    const LC_ApiError notEmpty = errorOf([&] { d.layers().remove(QStringLiteral("doors"), {}, audit); });
    CHECK(notEmpty.status == 409);
    CHECK(notEmpty.token == QStringLiteral("layer_not_empty"));
    CHECK(notEmpty.extra.value(QStringLiteral("entity_count")).toInt() == 2);
    CHECK(errorOf([&] { d.layers().remove(QStringLiteral("doors"), query({{"entities", "burn"}}), audit); }).token == QStringLiteral("bad_param"));
    CHECK(errorOf([&] { d.layers().remove(QStringLiteral("doors"), query({{"entities", "move"}, {"move_to", "nope"}}), audit); }).token ==
          QStringLiteral("unknown_layer"));
    CHECK(errorOf([&] { d.layers().remove(QStringLiteral("doors"), query({{"move_to", "0"}}), audit); }).token == QStringLiteral("bad_param"));
    CHECK(d.graphic.findLayer(QStringLiteral("doors")) != nullptr);

    d.graphic.activateLayer(d.graphic.findLayer(QStringLiteral("doors")));
    const QJsonObject moved = d.layers().remove(QStringLiteral("doors"), query({{"entities", "move"}, {"move_to", "0"}}), audit);
    CHECK(moved.value(QStringLiteral("deleted")).toString() == QStringLiteral("doors"));
    CHECK(moved.value(QStringLiteral("entities_moved")).toInt() == 2);
    CHECK(moved.value(QStringLiteral("entities_deleted")).toInt() == 0);
    CHECK_FALSE(moved.value(QStringLiteral("undoable")).toBool(true));
    CHECK(d.graphic.findLayer(QStringLiteral("doors")) == nullptr);
    CHECK(d.graphic.getActiveLayer()->getName() == QStringLiteral("0"));
    CHECK(audit.startsWith(QStringLiteral("[MCP] deleted layer 'doors'")));
    // the entities kept their ids and are on "0"
    CHECK(d.ops().get(a).value(QStringLiteral("entity")).toObject().value(QStringLiteral("layer")).toString() == QStringLiteral("0"));
    CHECK(d.ops().get(b).value(QStringLiteral("entity")).toObject().value(QStringLiteral("layer")).toString() == QStringLiteral("0"));
    // the move is one undo step; what comes back is on "0", never on the freed layer
    CHECK(d.graphic.undo());
    CHECK(d.liveCount() == 2);
    for (const RS_Entity* e : d.graphic) {
        const RS_Layer* layer = e->getLayer(false);
        REQUIRE(layer != nullptr);
        CHECK(layer->getName() == QStringLiteral("0"));
    }
    CHECK(d.graphic.redo());

    // delete with the entities: undoable, and they come back on "0"
    d.addLayer(QStringLiteral("trash"));
    d.createOne(R"({"entities":[{"type":"point","position":[5,5],"layer":"trash"}]})");
    const int before = d.liveCount();
    const QJsonObject deleted = d.layers().remove(QStringLiteral("trash"), query({{"entities", "delete"}}), audit);
    CHECK(deleted.value(QStringLiteral("entities_deleted")).toInt() == 1);
    CHECK(d.liveCount() == before - 1);
    CHECK(d.graphic.findLayer(QStringLiteral("trash")) == nullptr);
    CHECK(d.graphic.undo());
    CHECK(d.liveCount() == before);

    // an empty layer goes with the default policy
    d.addLayer(QStringLiteral("empty"));
    CHECK(d.layers().remove(QStringLiteral("empty"), {}, audit).value(QStringLiteral("deleted")).toString() == QStringLiteral("empty"));
}

TEST_CASE("Remote API blocks: list and insert", "[remote-api][blocks]") {
    Drawing d;
    QString audit;
    QJsonArray warnings;
    auto* block = new RS_Block(&d.graphic, RS_BlockData(QStringLiteral("door"), RS_Vector(1.0, 2.0), false));
    block->addEntity(new RS_Line(block, RS_LineData(RS_Vector(0.0, 0.0), RS_Vector(1.0, 0.0))));
    block->addEntity(new RS_Line(block, RS_LineData(RS_Vector(1.0, 0.0), RS_Vector(1.0, 2.0))));
    d.graphic.addBlock(block);

    QJsonArray blocks = d.layers().listBlocks().value(QStringLiteral("blocks")).toArray();
    REQUIRE(blocks.size() == 1);
    QJsonObject door = blocks.at(0).toObject();
    CHECK(door.value(QStringLiteral("name")).toString() == QStringLiteral("door"));
    CHECK(nearPoint(door.value(QStringLiteral("base_point")), 1.0, 2.0));
    CHECK(door.value(QStringLiteral("entity_count")).toInt() == 2);
    CHECK(door.value(QStringLiteral("insert_count")).toInt() == 0);
    CHECK_FALSE(door.value(QStringLiteral("frozen")).toBool());

    d.addLayer(QStringLiteral("furniture"));
    const QJsonObject inserted = d.layers().insertBlock(
        QStringLiteral("door"), LC_JsonReader(object(R"({"position":[10,10],"scale":2,"angle_deg":90,"layer":"furniture","select":true})")), warnings,
        audit);
    const QJsonObject entity = inserted.value(QStringLiteral("entity")).toObject();
    CHECK(entity.value(QStringLiteral("type")).toString() == QStringLiteral("insert"));
    CHECK(entity.value(QStringLiteral("block")).toString() == QStringLiteral("door"));
    CHECK(entity.value(QStringLiteral("layer")).toString() == QStringLiteral("furniture"));
    CHECK(nearPoint(entity.value(QStringLiteral("position")), 10.0, 10.0));
    CHECK(nearPoint(entity.value(QStringLiteral("scale")), 2.0, 2.0));
    CHECK(entity.value(QStringLiteral("selected")).toBool());
    CHECK(inserted.value(QStringLiteral("undoable")).toBool());
    CHECK(audit == QStringLiteral("[MCP] inserted block 'door' on layer 'furniture'"));
    CHECK(d.layers().listBlocks().value(QStringLiteral("blocks")).toArray().at(0).toObject().value(QStringLiteral("insert_count")).toInt() == 1);

    const LC_ApiError unknown = errorOf([&] { d.layers().insertBlock(QStringLiteral("window"), LC_JsonReader(object(R"({"position":[0,0]})")), warnings, audit); });
    CHECK(unknown.status == 404);
    CHECK(unknown.token == QStringLiteral("unknown_block"));
    CHECK(errorOf([&] { d.layers().insertBlock(QStringLiteral("door"), LC_JsonReader(object(R"({"position":[0,0],"block":"x"})")), warnings, audit); })
              .token == QStringLiteral("unknown_field"));
    CHECK(errorOf([&] { d.layers().insertBlock(QStringLiteral("door"), LC_JsonReader(object(R"({"position":[0,0],"scale":0})")), warnings, audit); })
              .token == QStringLiteral("invalid_entity"));

    // a moved block reference keeps its id; copies of it get ids of their own (from LC_UndoSection)
    const qint64 insertId = entity.value(QStringLiteral("id")).toInteger();
    const auto transform = [&](const QString& body) {
        return d.ops().transform(LC_JsonReader(object(body.toUtf8().constData())), audit);
    };
    const QJsonObject moved = transform(QStringLiteral(R"({"ids":[%1],"op":"move","offset":[1,0]})").arg(insertId))
                                  .value(QStringLiteral("entities")).toArray().at(0).toObject();
    CHECK(moved.value(QStringLiteral("id")).toInteger() == insertId);
    CHECK(nearPoint(moved.value(QStringLiteral("position")), 11.0, 10.0));
    const QJsonArray copyIds = transform(QStringLiteral(R"({"ids":[%1],"op":"move","offset":[0,5],"copy":true,"copies":2})").arg(insertId))
                                   .value(QStringLiteral("ids")).toArray();
    REQUIRE(copyIds.size() == 2);
    CHECK(copyIds.at(0).toInteger() != insertId);
    CHECK(copyIds.at(1).toInteger() != insertId);
    CHECK(copyIds.at(0).toInteger() != copyIds.at(1).toInteger());
    CHECK(d.liveCount() == 3);
    CHECK(d.ops().get(static_cast<unsigned long long>(insertId)).value(QStringLiteral("entity")).toObject().value(QStringLiteral("type")).toString() ==
          QStringLiteral("insert"));

    // copies, move, insert: three undo steps
    CHECK(d.graphic.undo());
    CHECK(d.liveCount() == 1);
    CHECK(d.graphic.undo());
    CHECK(nearPoint(d.ops().get(static_cast<unsigned long long>(insertId)).value(QStringLiteral("entity")).toObject().value(QStringLiteral("position")),
                    10.0, 10.0));
    CHECK(d.graphic.undo());
    CHECK(d.liveCount() == 0);
}

TEST_CASE("Remote API entities: every route of the component is registered", "[remote-api][entities][routes]") {
    LC_RemoteApiRouter router;
    lcRegisterEntityRoutes(router);
    QSet<QString> routes;
    QHash<QString, QString> capability;
    for (const QJsonValue& v : router.describe()) {
        const QJsonObject route = v.toObject();
        const QString key = route.value(QStringLiteral("method")).toString() + QLatin1Char(' ') + route.value(QStringLiteral("path")).toString();
        routes.insert(key);
        capability.insert(key, route.value(QStringLiteral("capability")).toString());
    }
    const QStringList expected{
        QStringLiteral("GET /api/v1/layers"),           QStringLiteral("POST /api/v1/layers"),
        QStringLiteral("PATCH /api/v1/layers/{name}"),  QStringLiteral("POST /api/v1/layers/{name}/current"),
        QStringLiteral("DELETE /api/v1/layers/{name}"), QStringLiteral("GET /api/v1/blocks"),
        QStringLiteral("POST /api/v1/blocks/{name}/insert"), QStringLiteral("GET /api/v1/entities"),
        QStringLiteral("GET /api/v1/entities/{id}"),    QStringLiteral("POST /api/v1/entities"),
        QStringLiteral("PATCH /api/v1/entities/{id}"),  QStringLiteral("DELETE /api/v1/entities/{id}"),
        QStringLiteral("POST /api/v1/entities/delete"), QStringLiteral("POST /api/v1/entities/transform"),
        QStringLiteral("GET /api/v1/selection"),        QStringLiteral("POST /api/v1/selection"),
        QStringLiteral("POST /api/v1/measure")};
    for (const QString& route : expected) {
        INFO(route.toStdString());
        CHECK(routes.contains(route));
    }
    CHECK(routes.size() == expected.size());
    CHECK(capability.value(QStringLiteral("POST /api/v1/entities/transform")) == QStringLiteral("transform"));
    CHECK(capability.value(QStringLiteral("POST /api/v1/blocks/{name}/insert")) == QStringLiteral("blocks"));
    CHECK(capability.value(QStringLiteral("POST /api/v1/measure")) == QStringLiteral("measure"));
    const QStringList capabilities = router.capabilities();
    for (const char* name : {"entities", "transform", "selection", "measure", "layers", "blocks"}) {
        CHECK(capabilities.contains(QLatin1String(name)));
    }
}

TEST_CASE("Remote API entities: handlers read the request and fill the call", "[remote-api][entities][routes]") {
    namespace routes = LC_RemoteApiEntityRoutes;
    Drawing d;

    HandlerCall create(d, "POST", QStringLiteral("/api/v1/entities"),
                       R"({"entities":[{"type":"line","start":[0,0],"end":[3,4]},{"type":"circle","center":[0,0],"radius":1}]})");
    LC_ApiResponse response = create.run(routes::createEntities);
    CHECK(response.status == 201);
    CHECK(response.json.value(QStringLiteral("ids")).toArray().size() == 2);
    CHECK(create.call->audit == QStringLiteral("[MCP] created 2 entities on layer '0'"));
    const qint64 line = response.json.value(QStringLiteral("ids")).toArray().at(0).toInteger();

    HandlerCall list(d, "GET", QStringLiteral("/api/v1/entities"), {}, query({{"type", "line"}, {"doc", "1"}}));
    response = list.run(routes::listEntities);
    CHECK(response.status == 200);
    CHECK(response.json.value(QStringLiteral("total")).toInt() == 1);

    HandlerCall get(d, "GET", QStringLiteral("/api/v1/entities/%1").arg(line), {}, {}, {{QStringLiteral("id"), QString::number(line)}});
    response = get.run(routes::getEntity);
    CHECK(response.json.value(QStringLiteral("entity")).toObject().value(QStringLiteral("length")).toDouble() == 5.0);

    HandlerCall badId(d, "GET", QStringLiteral("/api/v1/entities/abc"), {}, {}, {{QStringLiteral("id"), QStringLiteral("abc")}});
    response = badId.run(routes::getEntity);
    CHECK(response.status == 400);
    CHECK(response.json.value(QStringLiteral("error")).toString() == QStringLiteral("bad_param"));

    HandlerCall patch(d, "PATCH", QStringLiteral("/api/v1/entities/%1").arg(line), R"({"end":[6,8]})", {}, {{QStringLiteral("id"), QString::number(line)}});
    response = patch.run(routes::updateEntity);
    CHECK(response.status == 200);
    CHECK(response.json.value(QStringLiteral("entity")).toObject().value(QStringLiteral("length")).toDouble() == 10.0);

    HandlerCall invalidJson(d, "POST", QStringLiteral("/api/v1/entities"), "{not json");
    response = invalidJson.run(routes::createEntities);
    CHECK(response.status == 400);
    CHECK(response.json.value(QStringLiteral("error")).toString() == QStringLiteral("invalid_json"));

    HandlerCall extraQuery(d, "POST", QStringLiteral("/api/v1/measure"), R"({"kind":"distance","from":[0,0],"to":[1,0]})", query({{"x", "1"}}));
    response = extraQuery.run(routes::measure);
    CHECK(response.status == 400);
    CHECK(response.json.value(QStringLiteral("error")).toString() == QStringLiteral("unknown_field"));

    HandlerCall drop(d, "POST", QStringLiteral("/api/v1/entities/delete"), QStringLiteral(R"({"ids":[%1]})").arg(line).toUtf8());
    response = drop.run(routes::deleteEntities);
    CHECK(response.json.value(QStringLiteral("deleted")).toArray().size() == 1);
    CHECK(drop.call->audit == QStringLiteral("[MCP] deleted 1 entity"));

    HandlerCall layer(d, "POST", QStringLiteral("/api/v1/layers"), R"({"name":"tmp"})");
    response = layer.run(routes::createLayer);
    CHECK(response.status == 201);
    HandlerCall dropLayer(d, "DELETE", QStringLiteral("/api/v1/layers/tmp"), {}, {}, {{QStringLiteral("name"), QStringLiteral("tmp")}});
    response = dropLayer.run(routes::deleteLayer);
    CHECK(response.status == 200);
    CHECK(dropLayer.call->forceAudit);
    CHECK(dropLayer.call->audit == QStringLiteral("[MCP] deleted layer 'tmp'"));

    HandlerCall selection(d, "GET", QStringLiteral("/api/v1/selection"));
    CHECK(selection.run(routes::getSelection).json.value(QStringLiteral("count")).toInt() == 0);
}
