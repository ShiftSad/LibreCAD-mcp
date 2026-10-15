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

// Entities of the Remote API (design section 5.1, "Entities"): specs of every
// type, validation, atomic batches, undo, PATCH, delete, transforms, locked
// layers, queries, ids, measure and selection.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <iterator>

#include <QSet>

#include "lc_remoteapi_entities_testsupport.h"
#include "lc_remoteapientityjson.h"
#include "lc_sourcefonts.h"
#include "rs_block.h"
#include "rs_insert.h"
#include "rs_line.h"
#include "rs_math.h"

using namespace lc::test::remoteapi;

namespace {

void addBlock(RS_Graphic& graphic, const QString& name) {
    auto* block = new RS_Block(&graphic, RS_BlockData(name, RS_Vector(0.0, 0.0), false));
    block->addEntity(new RS_Line(block, RS_LineData(RS_Vector(0.0, 0.0), RS_Vector(1.0, 0.0))));
    block->addEntity(new RS_Line(block, RS_LineData(RS_Vector(1.0, 0.0), RS_Vector(1.0, 1.0))));
    graphic.addBlock(block);
}

/** Every field of the spec (but type) has the same value in the entity. */
void requireRoundTrip(const QJsonObject& spec, const QJsonObject& entity) {
    for (auto it = spec.constBegin(); it != spec.constEnd(); ++it) {
        // boundaries are input only: a hatch reports area and loop_count
        if (it.key() == QLatin1String("type") || it.key() == QLatin1String("boundary") || it.key() == QLatin1String("boundary_ids")) {
            continue;
        }
        INFO("field " << it.key().toStdString() << " of " << compact(spec).toStdString() << " -> "
                      << compact(entity).toStdString());
        REQUIRE(entity.contains(it.key()));
        CHECK(sameJson(it.value(), entity.value(it.key())));
    }
}

QJsonObject firstCreated(const QJsonObject& response) {
    const QJsonArray created = response.value(QStringLiteral("created")).toArray();
    REQUIRE(created.size() == 1);
    return created.at(0).toObject();
}

} // namespace

TEST_CASE("Remote API: one spec per type creates an entity that reads back its spec", "[remote-api][entities]") {
    Drawing d;
    lc::test::SourceFonts fonts;
    addBlock(d.graphic, QStringLiteral("B1"));

    const char* specs[] = {
        R"({"type":"line","start":[0,0],"end":[10,5],"color":"#ff0000","linetype":"DASHED","lineweight":0.25})",
        R"({"type":"circle","center":[1,2],"radius":3})",
        R"({"type":"arc","center":[0,0],"radius":5,"start_angle_deg":30,"end_angle_deg":120,"reversed":false})",
        R"({"type":"ellipse","center":[0,0],"major_axis":[10,0],"ratio":0.5,"start_angle_deg":0,"end_angle_deg":360,"reversed":false})",
        R"({"type":"ellipse","center":[0,0],"major_axis":[0,8],"ratio":0.25,"start_angle_deg":10,"end_angle_deg":100,"reversed":false})",
        R"({"type":"point","position":[7,8]})",
        R"({"type":"polyline","vertices":[[0,0],[10,0,0.5],[10,10],[0,10]],"closed":true})",
        R"({"type":"polyline","vertices":[[0,0],[5,5],[10,0]],"closed":false})",
        R"({"type":"spline","control_points":[[0,0],[5,10],[10,-10],[15,0]],"degree":3,"closed":false})",
        R"({"type":"spline","fit_points":[[0,0],[5,5],[10,0]],"closed":false})",
        R"({"type":"text","position":[1,1],"text":"Hello","height":2.5,"angle_deg":15,"halign":"left","valign":"baseline","style":"standard","width_factor":1})",
        R"({"type":"mtext","position":[2,3],"text":"Two words","height":3,"width":50,"angle_deg":0,"attachment":"top_left","line_spacing":1,"style":"standard"})",
        R"({"type":"dimension","dim_type":"linear","p1":[0,0],"p2":[10,0],"line_point":[10,5],"angle_deg":0,"text":""})",
        R"({"type":"dimension","dim_type":"aligned","p1":[0,0],"p2":[10,0],"line_point":[10,4]})",
        R"({"type":"dimension","dim_type":"radial","center":[0,0],"point":[5,0],"leader_length":0})",
        R"({"type":"dimension","dim_type":"diametric","point":[5,0],"opposite_point":[-5,0]})",
        R"({"type":"hatch","pattern":"SOLID","scale":1,"angle_deg":0,"boundary":[[[0,0],[10,0],[10,10],[0,10]]]})",
        R"({"type":"insert","block":"B1","position":[5,5],"scale":[2,2],"angle_deg":30,"columns":1,"rows":1,"spacing":[0,0]})",
    };
    for (const char* text : specs) {
        const QJsonObject spec = object(text);
        const QString body = QStringLiteral(R"({"entities":[%1]})").arg(QString::fromUtf8(text));
        const QJsonObject entity = firstCreated(d.create(body.toUtf8().constData()));
        CHECK(entity.value(QStringLiteral("type")).toString() == spec.value(QStringLiteral("type")).toString());
        requireRoundTrip(spec, entity);
        // GET /entities/{id} gives the same entity
        const unsigned long long id = static_cast<unsigned long long>(entity.value(QStringLiteral("id")).toInteger());
        CHECK(d.ops().get(id).value(QStringLiteral("entity")).toObject() == entity);
    }
    CHECK(d.liveCount() == static_cast<int>(std::size(specs)));

    // derived fields
    const QJsonObject line = d.ops().list(QMultiHash<QString, QString>{{QStringLiteral("type"), QStringLiteral("line")}})
                                 .value(QStringLiteral("entities")).toArray().at(0).toObject();
    CHECK(near(line.value(QStringLiteral("length")).toDouble(), std::hypot(10.0, 5.0)));
    CHECK(line.value(QStringLiteral("layer")).toString() == QStringLiteral("0"));

    QMultiHash<QString, QString> dims{{QStringLiteral("type"), QStringLiteral("dimension")}, {QStringLiteral("detail"), QStringLiteral("full")}};
    const QJsonArray dimensions = d.ops().list(dims).value(QStringLiteral("entities")).toArray();
    REQUIRE(dimensions.size() == 4);
    CHECK(near(dimensions.at(0).toObject().value(QStringLiteral("measurement")).toDouble(), 10.0));
    CHECK(near(dimensions.at(1).toObject().value(QStringLiteral("measurement")).toDouble(), 10.0));
    CHECK(near(dimensions.at(2).toObject().value(QStringLiteral("measurement")).toDouble(), 5.0));
    CHECK(near(dimensions.at(3).toObject().value(QStringLiteral("measurement")).toDouble(), 10.0));

    const QJsonObject hatch = d.ops().list(QMultiHash<QString, QString>{{QStringLiteral("type"), QStringLiteral("hatch")}})
                                  .value(QStringLiteral("entities")).toArray().at(0).toObject();
    CHECK(near(hatch.value(QStringLiteral("area")).toDouble(), 100.0, 1e-6));
    CHECK(hatch.value(QStringLiteral("loop_count")).toInt() == 1);
}

TEST_CASE("Remote API: angular dimensions measure the sector of arc_point", "[remote-api][entities]") {
    Drawing d;
    const auto measured = [&d](const char* body) {
        const QJsonObject entity = firstCreated(d.create(body));
        CHECK(entity.value(QStringLiteral("dim_type")).toString() == QStringLiteral("angular"));
        return entity.value(QStringLiteral("measurement")).toDouble();
    };
    CHECK(near(measured(R"({"entities":[{"type":"dimension","dim_type":"angular","line1":[[0,0],[10,0]],"line2":[[0,0],[0,10]],"arc_point":[3,3]}]})"),
               90.0, 1e-6));
    CHECK(near(measured(R"({"entities":[{"type":"dimension","dim_type":"angular","line1":[[0,0],[10,0]],"line2":[[0,0],[0,10]],"arc_point":[-3,3]}]})"),
               90.0, 1e-6));
    // two lines 30 degrees apart, crossing at (5, 0)
    const double c = 10.0 * std::cos(M_PI / 6.0);
    const double s = 10.0 * std::sin(M_PI / 6.0);
    const QString acute = QStringLiteral(R"({"entities":[{"type":"dimension","dim_type":"angular","line1":[[5,0],[15,0]],"line2":[[5,0],[%1,%2]],"arc_point":[12,1]}]})")
                              .arg(QString::number(5.0 + c, 'g', 17), QString::number(s, 'g', 17));
    CHECK(near(measured(acute.toUtf8().constData()), 30.0, 1e-6));
    const QString obtuse = QStringLiteral(R"({"entities":[{"type":"dimension","dim_type":"angular","line1":[[5,0],[15,0]],"line2":[[5,0],[%1,%2]],"arc_point":[3,4]}]})")
                               .arg(QString::number(5.0 + c, 'g', 17), QString::number(s, 'g', 17));
    CHECK(near(measured(obtuse.toUtf8().constData()), 150.0, 1e-6));

    const LC_ApiError parallel = errorOf([&d] {
        d.create(R"({"entities":[{"type":"dimension","dim_type":"angular","line1":[[0,0],[10,0]],"line2":[[0,5],[10,5]],"arc_point":[3,3]}]})");
    });
    CHECK(parallel.token == QStringLiteral("invalid_entity"));
    CHECK(parallel.extra.value(QStringLiteral("errors")).toObject().contains(QStringLiteral("entities[0].line2")));
}

TEST_CASE("Remote API: invalid specs are reported with their path and nothing is created", "[remote-api][entities]") {
    Drawing d;
    const LC_ApiError e = errorOf([&d] {
        d.create(R"({"entities":[
            {"type":"circle","center":[0,0],"radius":0},
            {"type":"polyline","vertices":[[0,0]]},
            {"type":"spline","control_points":[[0,0],[1,1],[2,0]],"degree":3},
            {"type":"blob"},
            {"type":"circle","center":[0,0],"radius":1,"start":[0,0]},
            {"type":"line","start":[0,0],"end":[1,1],"color":300},
            {"type":"line","start":[0,0],"end":[1,1],"lineweight":0.33},
            {"type":"line","start":[0,0],"end":[1,1]}
        ]})");
    });
    CHECK(e.status == 422);
    CHECK(e.token == QStringLiteral("invalid_entity"));
    const QJsonObject errors = e.extra.value(QStringLiteral("errors")).toObject();
    INFO(compact(errors).toStdString());
    CHECK(errors.contains(QStringLiteral("entities[0].radius")));
    CHECK(errors.contains(QStringLiteral("entities[1].vertices")));
    CHECK(errors.contains(QStringLiteral("entities[2].control_points")));
    CHECK(errors.contains(QStringLiteral("entities[3].type")));
    CHECK(errors.contains(QStringLiteral("entities[4].start")));
    CHECK(errors.contains(QStringLiteral("entities[5].color")));
    CHECK(errors.contains(QStringLiteral("entities[6].lineweight")));
    CHECK(errors.size() == 7);
    const QJsonObject allowed = e.extra.value(QStringLiteral("allowed")).toObject();
    const QJsonArray widths = allowed.value(QStringLiteral("entities[6].lineweight")).toArray();
    CHECK(widths.contains(QJsonValue(0.35)));
    CHECK(widths.contains(QJsonValue(QStringLiteral("ByLayer"))));
    CHECK(d.liveCount() == 0);

    // shape errors outside the specs are plain 400s
    CHECK(errorOf([&d] { d.create(R"({"entities":[],"bogus":1})"); }).token == QStringLiteral("unknown_field"));
    CHECK(errorOf([&d] { d.create(R"({"entities":[]})"); }).token == QStringLiteral("bad_param"));
    CHECK(errorOf([&d] { d.create(R"({})"); }).token == QStringLiteral("missing_param"));
    // references fail as themselves
    const LC_ApiError block = errorOf([&d] { d.create(R"({"entities":[{"type":"insert","block":"nope","position":[0,0]}]})"); });
    CHECK(block.status == 404);
    CHECK(block.token == QStringLiteral("unknown_block"));
    const LC_ApiError layer = errorOf([&d] { d.create(R"({"entities":[{"type":"point","position":[0,0],"layer":"nope"}]})"); });
    CHECK(layer.status == 404);
    CHECK(layer.token == QStringLiteral("unknown_layer"));
    CHECK(d.liveCount() == 0);
}

TEST_CASE("Remote API: a batch with one bad spec creates nothing", "[remote-api][entities]") {
    Drawing d;
    d.create(R"({"entities":[{"type":"point","position":[0,0]}]})");
    const LC_ApiError e = errorOf([&d] {
        d.create(R"({"entities":[
            {"type":"line","start":[0,0],"end":[1,0]},
            {"type":"circle","center":[0,0],"radius":-1},
            {"type":"point","position":[3,3]}]})");
    });
    CHECK(e.token == QStringLiteral("invalid_entity"));
    CHECK(d.liveCount() == 1);
    // the failed request left no undo step behind: one undo removes the point
    CHECK(d.graphic.undo());
    CHECK(d.liveCount() == 0);
    CHECK_FALSE(d.graphic.undo());
}

TEST_CASE("Remote API: a request is one undo step", "[remote-api][entities]") {
    Drawing d;
    QString specs;
    for (int i = 0; i < 10; ++i) {
        specs += QStringLiteral(R"(%1{"type":"line","start":[0,%2],"end":[10,%2]})").arg(i == 0 ? "" : ",").arg(i);
    }
    const QJsonObject result = d.create(QStringLiteral(R"({"entities":[%1],"select":true})").arg(specs).toUtf8().constData());
    CHECK(result.value(QStringLiteral("undoable")).toBool());
    CHECK(result.value(QStringLiteral("ids")).toArray().size() == 10);
    CHECK(d.liveCount() == 10);
    CHECK(d.ops().selection().value(QStringLiteral("count")).toInt() == 10);
    CHECK(d.graphic.undo());
    CHECK(d.liveCount() == 0);
    CHECK_FALSE(d.graphic.undo());
    CHECK(d.graphic.redo());
    CHECK(d.liveCount() == 10);
}

TEST_CASE("Remote API: entities get a ByLayer pen, the current layer and audit lines", "[remote-api][entities]") {
    Drawing d;
    RS_Layer* walls = d.addLayer(QStringLiteral("walls"));
    d.graphic.activateLayer(walls);
    QJsonArray warnings;
    QString audit;
    const QJsonObject result = d.ops().create(LC_JsonReader(object(R"({"entities":[{"type":"point","position":[0,0]},{"type":"point","position":[1,0]}]})")),
                                              warnings, audit);
    const QJsonObject point = result.value(QStringLiteral("created")).toArray().at(0).toObject();
    CHECK(point.value(QStringLiteral("layer")).toString() == QStringLiteral("walls"));
    CHECK(point.value(QStringLiteral("color")).toString() == QStringLiteral("ByLayer"));
    CHECK(point.value(QStringLiteral("linetype")).toString() == QStringLiteral("ByLayer"));
    CHECK(point.value(QStringLiteral("lineweight")).toString() == QStringLiteral("ByLayer"));
    CHECK(audit == QStringLiteral("[MCP] created 2 entities on layer 'walls'"));

    // the body layer applies to specs without one; a frozen layer warns
    RS_Layer* frozen = d.addLayer(QStringLiteral("frozen"));
    frozen->freeze(true);
    warnings = {};
    const QJsonObject onFrozen = d.ops().create(
        LC_JsonReader(object(R"({"layer":"frozen","entities":[{"type":"point","position":[0,0]},{"type":"point","position":[0,0],"layer":"0"}]})")),
        warnings, audit);
    const QJsonArray created = onFrozen.value(QStringLiteral("created")).toArray();
    CHECK(created.at(0).toObject().value(QStringLiteral("layer")).toString() == QStringLiteral("frozen"));
    CHECK(created.at(1).toObject().value(QStringLiteral("layer")).toString() == QStringLiteral("0"));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.at(0).toString().startsWith(QStringLiteral("layer_frozen")));
}

TEST_CASE("Remote API: PATCH keeps the id; a delete is undone with the same id", "[remote-api][entities]") {
    Drawing d;
    const unsigned long long id = d.createOne(R"({"entities":[{"type":"line","start":[0,0],"end":[10,0]}]})");
    QJsonArray warnings;
    QString audit;

    const QJsonObject patched = d.ops().update(id, LC_JsonReader(object(R"({"end":[20,0],"color":"#00ff00"})")), warnings, audit);
    const QJsonObject entity = patched.value(QStringLiteral("entity")).toObject();
    CHECK(static_cast<unsigned long long>(entity.value(QStringLiteral("id")).toInteger()) == id);
    CHECK(nearPoint(entity.value(QStringLiteral("end")), 20.0, 0.0));
    CHECK(nearPoint(entity.value(QStringLiteral("start")), 0.0, 0.0));
    CHECK(entity.value(QStringLiteral("color")).toString() == QStringLiteral("#00ff00"));
    CHECK(d.liveCount() == 1);
    CHECK(d.entity(id) != nullptr);

    // undo brings the old geometry back under the same id
    CHECK(d.graphic.undo());
    REQUIRE(d.entity(id) != nullptr);
    CHECK(d.entity(id)->getEndpoint().x == 10.0);
    CHECK(d.graphic.redo());

    // attributes only, and geometry of a container type
    const unsigned long long polyline = d.createOne(R"({"entities":[{"type":"polyline","vertices":[[0,0],[1,0],[1,1]]}]})");
    const QJsonObject closed = d.ops().update(polyline, LC_JsonReader(object(R"({"closed":true,"lineweight":0.5})")), warnings, audit)
                                   .value(QStringLiteral("entity")).toObject();
    CHECK(static_cast<unsigned long long>(closed.value(QStringLiteral("id")).toInteger()) == polyline);
    CHECK(closed.value(QStringLiteral("closed")).toBool());
    CHECK(near(closed.value(QStringLiteral("lineweight")).toDouble(), 0.5));
    CHECK(near(closed.value(QStringLiteral("area")).toDouble(), 0.5));

    // text keeps the id through setters
    const unsigned long long circle = d.createOne(R"({"entities":[{"type":"circle","center":[0,0],"radius":1}]})");
    CHECK(d.ops().update(circle, LC_JsonReader(object(R"({"radius":4})")), warnings, audit)
              .value(QStringLiteral("entity")).toObject().value(QStringLiteral("radius")).toDouble() == 4.0);

    // a field of another type, a type change, nothing to change
    const LC_ApiError foreign = errorOf([&] { d.ops().update(circle, LC_JsonReader(object(R"({"start":[0,0]})")), warnings, audit); });
    CHECK(foreign.status == 400);
    CHECK(foreign.token == QStringLiteral("unknown_field"));
    CHECK(foreign.extra.value(QStringLiteral("field")).toString() == QStringLiteral("start"));
    const LC_ApiError retype = errorOf([&] { d.ops().update(circle, LC_JsonReader(object(R"({"type":"line"})")), warnings, audit); });
    CHECK(retype.token == QStringLiteral("bad_param"));
    const LC_ApiError invalid = errorOf([&] { d.ops().update(circle, LC_JsonReader(object(R"({"radius":0})")), warnings, audit); });
    CHECK(invalid.status == 422);
    CHECK(invalid.token == QStringLiteral("invalid_entity"));
    CHECK(errorOf([&] { d.ops().update(circle, LC_JsonReader(QJsonObject()), warnings, audit); }).token == QStringLiteral("bad_param"));
    CHECK(errorOf([&] { d.ops().update(987654321, LC_JsonReader(object(R"({"radius":2})")), warnings, audit); }).token ==
          QStringLiteral("unknown_entity"));

    // delete, then undo: the same id is back
    const int before = d.liveCount();
    const QJsonObject deleted = d.ops().remove({id}, audit);
    CHECK(deleted.value(QStringLiteral("deleted")).toArray().at(0).toInteger() == static_cast<qint64>(id));
    CHECK(deleted.value(QStringLiteral("undoable")).toBool());
    CHECK(d.liveCount() == before - 1);
    const LC_ApiError gone = errorOf([&] { d.ops().get(id); });
    CHECK(gone.status == 404);
    CHECK(gone.token == QStringLiteral("unknown_entity"));
    CHECK(d.graphic.undo());
    CHECK(d.liveCount() == before);
    CHECK(d.ops().get(id).value(QStringLiteral("entity")).toObject().value(QStringLiteral("type")).toString() == QStringLiteral("line"));

    // unknown ids: nothing is deleted
    const LC_ApiError unknown = errorOf([&] { d.ops().remove({id, 4242424242ULL}, audit); });
    CHECK(unknown.token == QStringLiteral("unknown_entity"));
    CHECK(unknown.extra.value(QStringLiteral("ids")).toArray() == QJsonArray{static_cast<qint64>(4242424242ULL)});
    CHECK(d.liveCount() == before);
}

TEST_CASE("Remote API: transforms keep ids, copies get new ones", "[remote-api][entities]") {
    Drawing d;
    QString audit;
    const auto transform = [&](const QString& body) {
        return d.ops().transform(LC_JsonReader(object(body.toUtf8().constData())), audit);
    };
    const auto first = [](const QJsonObject& result) {
        return result.value(QStringLiteral("entities")).toArray().at(0).toObject();
    };

    const unsigned long long line = d.createOne(R"({"entities":[{"type":"line","start":[0,0],"end":[10,0]}]})");
    QJsonObject moved = first(transform(QStringLiteral(R"({"ids":[%1],"op":"move","offset":[5,5]})").arg(line)));
    CHECK(static_cast<unsigned long long>(moved.value(QStringLiteral("id")).toInteger()) == line);
    CHECK(nearPoint(moved.value(QStringLiteral("start")), 5.0, 5.0));
    CHECK(nearPoint(moved.value(QStringLiteral("end")), 15.0, 5.0));
    CHECK(audit == QStringLiteral("[MCP] moved 1 entity"));

    const unsigned long long rotating = d.createOne(R"({"entities":[{"type":"line","start":[0,0],"end":[10,0]}]})");
    const QJsonObject rotated = first(transform(QStringLiteral(R"({"ids":[%1],"op":"rotate","center":[0,0],"angle_deg":90})").arg(rotating)));
    CHECK(nearPoint(rotated.value(QStringLiteral("end")), 0.0, 10.0));
    CHECK(static_cast<unsigned long long>(rotated.value(QStringLiteral("id")).toInteger()) == rotating);

    const unsigned long long scaling = d.createOne(R"({"entities":[{"type":"line","start":[0,0],"end":[10,10]}]})");
    const QJsonObject scaled = first(transform(QStringLiteral(R"({"ids":[%1],"op":"scale","base":[0,0],"factor":[2,1]})").arg(scaling)));
    CHECK(nearPoint(scaled.value(QStringLiteral("end")), 20.0, 10.0));

    const unsigned long long circle = d.createOne(R"({"entities":[{"type":"circle","center":[1,1],"radius":5}]})");
    const QJsonObject doubled = first(transform(QStringLiteral(R"({"ids":[%1],"op":"scale","base":[1,1],"factor":2})").arg(circle)));
    CHECK(near(doubled.value(QStringLiteral("radius")).toDouble(), 10.0));
    // a circle scaled non-isotropically becomes an ellipse, and keeps its id
    const QJsonObject stretched = first(transform(QStringLiteral(R"({"ids":[%1],"op":"scale","base":[1,1],"factor":[2,1]})").arg(circle)));
    CHECK(stretched.value(QStringLiteral("type")).toString() == QStringLiteral("ellipse"));
    CHECK(static_cast<unsigned long long>(stretched.value(QStringLiteral("id")).toInteger()) == circle);

    const unsigned long long mirroring = d.createOne(R"({"entities":[{"type":"line","start":[1,0],"end":[5,0]}]})");
    const QJsonObject mirrored = first(transform(QStringLiteral(R"({"ids":[%1],"op":"mirror","axis":[[0,0],[0,1]]})").arg(mirroring)));
    CHECK(nearPoint(mirrored.value(QStringLiteral("start")), -1.0, 0.0));
    CHECK(nearPoint(mirrored.value(QStringLiteral("end")), -5.0, 0.0));

    // three copies: three new distinct ids, the original kept
    const int before = d.liveCount();
    const QJsonObject copies = transform(QStringLiteral(R"({"ids":[%1],"op":"move","offset":[0,10],"copy":true,"copies":3})").arg(line));
    CHECK(copies.value(QStringLiteral("originals_kept")).toBool());
    const QJsonArray ids = copies.value(QStringLiteral("ids")).toArray();
    REQUIRE(ids.size() == 3);
    QSet<qint64> distinct;
    for (const QJsonValue& id : ids) {
        distinct.insert(id.toInteger());
    }
    CHECK(distinct.size() == 3);
    CHECK_FALSE(distinct.contains(static_cast<qint64>(line)));
    CHECK(d.liveCount() == before + 3);
    CHECK(d.entity(line) != nullptr);
    CHECK(nearPoint(copies.value(QStringLiteral("entities")).toArray().at(2).toObject().value(QStringLiteral("start")), 5.0, 35.0));
    // the copies are one undo step
    CHECK(d.graphic.undo());
    CHECK(d.liveCount() == before);

    // wrong shapes
    CHECK(errorOf([&] { transform(QStringLiteral(R"({"ids":[%1],"op":"rotate","offset":[1,1]})").arg(line)); }).token == QStringLiteral("bad_param"));
    CHECK(errorOf([&] { transform(QStringLiteral(R"({"ids":[%1],"op":"move","offset":[1,1],"copies":2})").arg(line)); }).token ==
          QStringLiteral("bad_param"));
    CHECK(errorOf([&] { transform(QStringLiteral(R"({"ids":[%1],"op":"twist"})").arg(line)); }).token == QStringLiteral("bad_param"));
    CHECK(errorOf([&] { transform(QStringLiteral(R"({"ids":[%1],"op":"move","offset":[1,1],"bogus":1})").arg(line)); }).token ==
          QStringLiteral("unknown_field"));
    CHECK(errorOf([&] { transform(QStringLiteral(R"({"ids":[999999],"op":"move","offset":[1,1]})")); }).token == QStringLiteral("unknown_entity"));
}

TEST_CASE("Remote API: locked layers are refused before anything changes", "[remote-api][entities]") {
    Drawing d;
    QString audit;
    QJsonArray warnings;
    RS_Layer* locked = d.addLayer(QStringLiteral("locked"));
    locked->lock(true);
    const LC_ApiError onLocked = errorOf([&d] { d.create(R"({"entities":[{"type":"point","position":[0,0],"layer":"locked"}]})"); });
    CHECK(onLocked.status == 409);
    CHECK(onLocked.token == QStringLiteral("layer_locked"));
    CHECK(onLocked.extra.value(QStringLiteral("layer")).toString() == QStringLiteral("locked"));
    CHECK(d.liveCount() == 0);

    RS_Layer* work = d.addLayer(QStringLiteral("work"));
    const unsigned long long line = d.createOne(R"({"entities":[{"type":"line","start":[0,0],"end":[10,0],"layer":"work"}]})");
    const unsigned long long loose = d.createOne(R"({"entities":[{"type":"line","start":[0,5],"end":[10,5]}]})");
    work->lock(true);
    const int before = d.liveCount();

    const LC_ApiError transform = errorOf([&] {
        d.ops().transform(LC_JsonReader(object(QStringLiteral(R"({"ids":[%1,%2],"op":"move","offset":[1,1]})").arg(line).arg(loose).toUtf8().constData())),
                          audit);
    });
    CHECK(transform.status == 409);
    CHECK(transform.token == QStringLiteral("entity_locked"));
    CHECK(transform.extra.value(QStringLiteral("ids")).toArray() == QJsonArray{static_cast<qint64>(line)});
    // nothing moved and nothing was added (LC_UndoSection would have added the clones)
    CHECK(d.liveCount() == before);
    CHECK(d.entity(loose)->getStartpoint().y == 5.0);
    const LC_ApiError copy = errorOf([&] {
        d.ops().transform(LC_JsonReader(object(QStringLiteral(R"({"ids":[%1],"op":"move","offset":[1,1],"copy":true})").arg(line).toUtf8().constData())),
                          audit);
    });
    CHECK(copy.token == QStringLiteral("entity_locked"));
    CHECK(d.liveCount() == before);

    CHECK(errorOf([&] { d.ops().remove({line}, audit); }).token == QStringLiteral("entity_locked"));
    CHECK(errorOf([&] { d.ops().update(line, LC_JsonReader(object(R"({"color":"#ff0000"})")), warnings, audit); }).token ==
          QStringLiteral("entity_locked"));
    CHECK(errorOf([&] { d.ops().update(loose, LC_JsonReader(object(R"({"layer":"work"})")), warnings, audit); }).token == QStringLiteral("layer_locked"));
    CHECK(d.liveCount() == before);
}

TEST_CASE("Remote API: queries filter by type, layer, area and selection, with paging", "[remote-api][entities]") {
    Drawing d;
    d.addLayer(QStringLiteral("A"));
    const unsigned long long inside = d.createOne(R"({"entities":[{"type":"line","start":[1,1],"end":[9,1]}]})");
    const unsigned long long crossing = d.createOne(R"({"entities":[{"type":"line","start":[-5,5],"end":[5,5]}]})");
    const unsigned long long far = d.createOne(R"({"entities":[{"type":"circle","center":[50,50],"radius":5,"layer":"A"}]})");
    const auto ids = [&d](const QMultiHash<QString, QString>& query) {
        QList<qint64> out;
        for (const QJsonValue& e : d.ops().list(query).value(QStringLiteral("entities")).toArray()) {
            out.append(e.toObject().value(QStringLiteral("id")).toInteger());
        }
        return out;
    };
    const auto q = [](std::initializer_list<std::pair<const char*, const char*>> pairs) {
        QMultiHash<QString, QString> out;
        for (const auto& [key, value] : pairs) {
            out.insert(QString::fromLatin1(key), QString::fromLatin1(value));
        }
        return out;
    };

    CHECK(ids(q({{"type", "line"}})) == QList<qint64>{qint64(inside), qint64(crossing)});
    CHECK(ids(q({{"type", "line,circle"}})).size() == 3);
    CHECK(ids(q({{"type", "line"}, {"type", "circle"}})).size() == 3);
    CHECK(ids(q({{"layer", "A"}})) == QList<qint64>{qint64(far)});
    CHECK(ids(q({{"bbox", "0,0,10,10"}, {"bbox_mode", "window"}})) == QList<qint64>{qint64(inside)});
    CHECK(ids(q({{"bbox", "0,0,10,10"}})) == QList<qint64>{qint64(inside), qint64(crossing)});
    CHECK(ids(q({{"bbox", "10,10,0,0"}, {"bbox_mode", "crossing"}})).size() == 2);

    // the selection
    QJsonObject selected = d.ops().select(LC_JsonReader(object(QStringLiteral(R"({"mode":"set","ids":[%1]})").arg(far).toUtf8().constData())));
    CHECK(selected.value(QStringLiteral("count")).toInt() == 1);
    CHECK(ids(q({{"selected", "true"}})) == QList<qint64>{qint64(far)});

    // paging
    for (int i = 0; i < 5; ++i) {
        d.create(R"({"entities":[{"type":"point","position":[100,100]}]})");
    }
    QJsonObject page = d.ops().list(q({{"type", "point"}, {"limit", "2"}}));
    CHECK(page.value(QStringLiteral("total")).toInt() == 5);
    CHECK(page.value(QStringLiteral("entities")).toArray().size() == 2);
    CHECK(page.value(QStringLiteral("next_offset")).toInt() == 2);
    page = d.ops().list(q({{"type", "point"}, {"limit", "2"}, {"offset", "4"}}));
    CHECK(page.value(QStringLiteral("entities")).toArray().size() == 1);
    CHECK(page.value(QStringLiteral("next_offset")).isNull());

    // deleted entities never appear, not even through the undo history
    QString audit;
    d.ops().remove({inside}, audit);
    CHECK_FALSE(ids(q({{"type", "line"}})).contains(qint64(inside)));
    CHECK(d.ops().list(q({{"include_hidden", "true"}})).value(QStringLiteral("total")).toInt() == 7);

    // frozen layers hide their entities unless include_hidden
    RS_Layer* layerA = d.graphic.findLayer(QStringLiteral("A"));
    d.graphic.setFreezeLayers({}, {layerA});
    CHECK(ids(q({{"layer", "A"}})).isEmpty());
    CHECK(ids(q({{"layer", "A"}, {"include_hidden", "true"}})) == QList<qint64>{qint64(far)});

    // summary detail cuts point arrays
    const unsigned long long polyline = d.createOne(R"({"entities":[{"type":"polyline","vertices":[[0,0],[1,0],[1,1]]}]})");
    const QJsonObject summary = d.ops().list(q({{"type", "polyline"}})).value(QStringLiteral("entities")).toArray().at(0).toObject();
    CHECK_FALSE(summary.contains(QStringLiteral("vertices")));
    CHECK(summary.value(QStringLiteral("vertex_count")).toInt() == 3);
    CHECK(d.ops().get(polyline).value(QStringLiteral("entity")).toObject().value(QStringLiteral("vertices")).toArray().size() == 3);

    // bad queries
    CHECK(errorOf([&] { d.ops().list(q({{"type", "blob"}})); }).token == QStringLiteral("bad_param"));
    CHECK(errorOf([&] { d.ops().list(q({{"layer", "nope"}})); }).token == QStringLiteral("unknown_layer"));
    CHECK(errorOf([&] { d.ops().list(q({{"bbox", "1,2,3"}})); }).token == QStringLiteral("bad_param"));
    CHECK(errorOf([&] { d.ops().list(q({{"limit", "1001"}})); }).token == QStringLiteral("bad_param"));
    CHECK(errorOf([&] { d.ops().list(q({{"colour", "red"}})); }).token == QStringLiteral("unknown_field"));
    CHECK(d.ops().list(q({{"doc", "1"}})).value(QStringLiteral("total")).toInt() >= 1);
}

TEST_CASE("Remote API: duplicate lineages are normalized, the original keeps its id", "[remote-api][entities]") {
    Drawing d;
    const unsigned long long id = d.createOne(R"({"entities":[{"type":"line","start":[0,0],"end":[10,0]}]})");
    RS_Entity* original = d.entity(id);
    REQUIRE(original != nullptr);
    // a plain copy, as a GUI paste makes it: the same lineage
    RS_Entity* copy = original->clone();
    copy->move(RS_Vector(0.0, 5.0));
    d.graphic.addEntity(copy);
    REQUIRE(copy->lineage() == original->lineage());

    const LC_RemoteApiEntityIndex index(&d.graphic);
    CHECK(index.normalizedCount() == 1);
    CHECK(index.find(id) == original);
    CHECK(copy->lineage() != id);
    CHECK(index.find(copy->lineage()) == copy);
    QSet<qint64> seen;
    for (const QJsonValue& e : d.ops().list({}).value(QStringLiteral("entities")).toArray()) {
        seen.insert(e.toObject().value(QStringLiteral("id")).toInteger());
    }
    CHECK(seen.size() == 2);

    // a copied block reference cannot be renewed: its id is ambiguous
    addBlock(d.graphic, QStringLiteral("B"));
    const unsigned long long insert = d.createOne(R"({"entities":[{"type":"insert","block":"B","position":[0,0]}]})");
    RS_Entity* insertCopy = d.entity(insert)->clone();
    d.graphic.addEntity(insertCopy);
    const LC_RemoteApiEntityIndex again(&d.graphic);
    CHECK(again.isAmbiguous(insert));
    const LC_ApiError ambiguous = errorOf([&] { d.ops().get(insert); });
    CHECK(ambiguous.status == 409);
    CHECK(ambiguous.token == QStringLiteral("ambiguous_id"));
}

TEST_CASE("Remote API: measure distances, lengths, areas, angles and intersections", "[remote-api][entities]") {
    Drawing d;
    const auto measure = [&d](const QString& body) {
        return d.ops().measure(LC_JsonReader(object(body.toUtf8().constData())));
    };
    QJsonObject r = measure(QStringLiteral(R"({"kind":"distance","from":[0,0],"to":[3,4]})"));
    CHECK(near(r.value(QStringLiteral("distance")).toDouble(), 5.0));
    CHECK(near(r.value(QStringLiteral("dx")).toDouble(), 3.0));
    CHECK(near(r.value(QStringLiteral("dy")).toDouble(), 4.0));
    CHECK(near(r.value(QStringLiteral("angle_deg")).toDouble(), RS_Math::rad2deg(std::atan2(4.0, 3.0))));

    const unsigned long long line = d.createOne(R"({"entities":[{"type":"line","start":[0,0],"end":[10,0]}]})");
    r = measure(QStringLiteral(R"({"kind":"distance","from":[5,5],"id":%1})").arg(line));
    CHECK(near(r.value(QStringLiteral("distance")).toDouble(), 5.0));
    CHECK(nearPoint(r.value(QStringLiteral("nearest_point")), 5.0, 0.0));

    const unsigned long long square = d.createOne(R"({"entities":[{"type":"polyline","vertices":[[0,0],[10,0],[10,10],[0,10]],"closed":true}]})");
    r = measure(QStringLiteral(R"({"kind":"area","ids":[%1]})").arg(square));
    CHECK(near(r.value(QStringLiteral("area")).toDouble(), 100.0));
    CHECK(near(r.value(QStringLiteral("perimeter")).toDouble(), 40.0));
    CHECK(r.value(QStringLiteral("per_entity")).toObject().contains(QString::number(square)));

    const unsigned long long circle = d.createOne(R"({"entities":[{"type":"circle","center":[0,0],"radius":2}]})");
    r = measure(QStringLiteral(R"({"kind":"area","ids":[%1]})").arg(circle));
    CHECK(near(r.value(QStringLiteral("area")).toDouble(), M_PI * 4.0));

    r = measure(QStringLiteral(R"({"kind":"area","points":[[0,0],[4,0],[4,3]]})"));
    CHECK(near(r.value(QStringLiteral("area")).toDouble(), 6.0));
    CHECK(near(r.value(QStringLiteral("perimeter")).toDouble(), 12.0));

    const unsigned long long open = d.createOne(R"({"entities":[{"type":"polyline","vertices":[[0,0],[10,0],[10,10]]}]})");
    const LC_ApiError notClosed = errorOf([&] { measure(QStringLiteral(R"({"kind":"area","ids":[%1,%2]})").arg(square).arg(open)); });
    CHECK(notClosed.status == 422);
    CHECK(notClosed.token == QStringLiteral("not_closed"));
    CHECK(notClosed.extra.value(QStringLiteral("ids")).toArray() == QJsonArray{static_cast<qint64>(open)});

    r = measure(QStringLiteral(R"({"kind":"length","ids":[%1,%2]})").arg(line).arg(open));
    CHECK(near(r.value(QStringLiteral("length")).toDouble(), 30.0));

    const unsigned long long a = d.createOne(R"({"entities":[{"type":"line","start":[0,0],"end":[10,10]}]})");
    const unsigned long long b = d.createOne(R"({"entities":[{"type":"line","start":[0,10],"end":[10,0]}]})");
    r = measure(QStringLiteral(R"({"kind":"intersections","ids":[%1,%2]})").arg(a).arg(b));
    const QJsonArray points = r.value(QStringLiteral("points")).toArray();
    REQUIRE(points.size() == 1);
    CHECK(nearPoint(points.at(0), 5.0, 5.0));
    // a line through a polyline square: two crossings
    const unsigned long long across = d.createOne(R"({"entities":[{"type":"line","start":[-5,5],"end":[15,5]}]})");
    r = measure(QStringLiteral(R"({"kind":"intersections","ids":[%1,%2]})").arg(across).arg(square));
    CHECK(r.value(QStringLiteral("points")).toArray().size() == 2);

    r = measure(QStringLiteral(R"({"kind":"angle","ids":[%1,%2]})").arg(a).arg(b));
    CHECK(near(r.value(QStringLiteral("angle_deg")).toDouble(), 90.0));
    r = measure(QStringLiteral(R"({"kind":"angle","points":[[0,0],[1,0],[1,1]]})"));
    CHECK(near(r.value(QStringLiteral("angle_deg")).toDouble(), 45.0));

    CHECK(errorOf([&] { measure(QStringLiteral(R"({"kind":"volume"})")); }).token == QStringLiteral("bad_param"));
    CHECK(errorOf([&] { measure(QStringLiteral(R"({"kind":"distance","from":[0,0]})")); }).token == QStringLiteral("bad_param"));
    CHECK(errorOf([&] { measure(QStringLiteral(R"({"kind":"angle","ids":[%1,%2]})").arg(a).arg(circle)); }).token == QStringLiteral("bad_param"));
}

TEST_CASE("Remote API: selection modes", "[remote-api][entities]") {
    Drawing d;
    d.addLayer(QStringLiteral("L"));
    const unsigned long long p1 = d.createOne(R"({"entities":[{"type":"point","position":[0,0]}]})");
    const unsigned long long p2 = d.createOne(R"({"entities":[{"type":"point","position":[10,10],"layer":"L"}]})");
    const unsigned long long l1 = d.createOne(R"({"entities":[{"type":"line","start":[20,20],"end":[30,20]}]})");
    const auto select = [&d](const QString& body) {
        return d.ops().select(LC_JsonReader(object(body.toUtf8().constData())));
    };
    const auto idSet = [](const QJsonObject& result) {
        QSet<qint64> out;
        for (const QJsonValue& v : result.value(QStringLiteral("ids")).toArray()) {
            out.insert(v.toInteger());
        }
        return out;
    };
    CHECK(select(QStringLiteral(R"({"mode":"all"})")).value(QStringLiteral("count")).toInt() == 3);
    CHECK(select(QStringLiteral(R"({"mode":"clear"})")).value(QStringLiteral("count")).toInt() == 0);
    CHECK(idSet(select(QStringLiteral(R"({"mode":"set","ids":[%1]})").arg(p1))) == QSet<qint64>{qint64(p1)});
    CHECK(idSet(select(QStringLiteral(R"({"mode":"add","type":["line"]})"))) == QSet<qint64>{qint64(p1), qint64(l1)});
    CHECK(idSet(select(QStringLiteral(R"({"mode":"remove","ids":[%1]})").arg(p1))) == QSet<qint64>{qint64(l1)});
    CHECK(idSet(select(QStringLiteral(R"({"mode":"set","layer":["L"]})"))) == QSet<qint64>{qint64(p2)});
    CHECK(idSet(select(QStringLiteral(R"({"mode":"set","bbox":[-1,-1,11,11],"bbox_mode":"window"})"))) == QSet<qint64>{qint64(p1), qint64(p2)});
    CHECK(idSet(d.ops().selection()) == QSet<qint64>{qint64(p1), qint64(p2)});
    CHECK(errorOf([&] { select(QStringLiteral(R"({"mode":"set"})")); }).token == QStringLiteral("missing_param"));
    CHECK(errorOf([&] { select(QStringLiteral(R"({"mode":"clear","ids":[%1]})").arg(p1)); }).token == QStringLiteral("bad_param"));
    CHECK(errorOf([&] { select(QStringLiteral(R"({"mode":"flip"})")); }).token == QStringLiteral("bad_param"));
}

TEST_CASE("Remote API: hatches from closed entities", "[remote-api][entities]") {
    Drawing d;
    const unsigned long long square = d.createOne(R"({"entities":[{"type":"polyline","vertices":[[0,0],[4,0],[4,4],[0,4]],"closed":true}]})");
    const QJsonObject hatch = firstCreated(d.create(QStringLiteral(R"({"entities":[{"type":"hatch","boundary_ids":[%1]}]})").arg(square).toUtf8().constData()));
    CHECK(near(hatch.value(QStringLiteral("area")).toDouble(), 16.0, 1e-6));
    CHECK(hatch.value(QStringLiteral("pattern")).toString() == QStringLiteral("SOLID"));
    // a loop with an arc (bulge)
    const QJsonObject rounded = firstCreated(d.create(R"({"entities":[{"type":"hatch","boundary":[{"vertices":[[0,0,1],[2,0,1]],"closed":true}]}]})"));
    CHECK(near(rounded.value(QStringLiteral("area")).toDouble(), M_PI, 1e-6));
    const unsigned long long hatchId = static_cast<unsigned long long>(hatch.value(QStringLiteral("id")).toInteger());
    const QJsonObject measured = d.ops().measure(LC_JsonReader(object(QStringLiteral(R"({"kind":"area","ids":[%1]})").arg(hatchId).toUtf8().constData())));
    CHECK(near(measured.value(QStringLiteral("area")).toDouble(), 16.0, 1e-6));
    CHECK(near(measured.value(QStringLiteral("perimeter")).toDouble(), 16.0, 1e-6));

    const LC_ApiError both = errorOf([&d] {
        d.create(R"({"entities":[{"type":"hatch"}]})");
    });
    CHECK(both.token == QStringLiteral("invalid_entity"));
    const LC_ApiError missing = errorOf([&d] { d.create(R"({"entities":[{"type":"hatch","boundary_ids":[123456789]}]})"); });
    CHECK(missing.token == QStringLiteral("unknown_entity"));
}

TEST_CASE("Remote API: entity json values", "[remote-api][entities]") {
    namespace json = LC_RemoteApiEntityJson;
    lc::test::application();
    CHECK(json::colorName(json::parseColor(QJsonValue(1), QStringLiteral("c"), true)) == QStringLiteral("#ff0000"));
    CHECK(json::colorName(json::parseColor(QJsonValue(QStringLiteral("bylayer")), QStringLiteral("c"), true)) == QStringLiteral("ByLayer"));
    CHECK(json::colorName(json::parseColor(QJsonValue(QStringLiteral("#A0b0C0")), QStringLiteral("c"), true)) == QStringLiteral("#a0b0c0"));
    CHECK(errorOf([] { json::parseColor(QJsonValue(QStringLiteral("ByLayer")), QStringLiteral("c"), false); }).token == QStringLiteral("bad_param"));
    CHECK(errorOf([] { json::parseColor(QJsonValue(0), QStringLiteral("c"), true); }).token == QStringLiteral("bad_param"));
    CHECK(json::aciNumber(json::parseColor(QJsonValue(5), QStringLiteral("c"), false)) == 5);
    CHECK(json::parseLineWidth(QJsonValue(0.254), QStringLiteral("w"), true) == RS2::Width07);
    CHECK(json::parseLineWidth(QJsonValue(QStringLiteral("Default")), QStringLiteral("w"), false) == RS2::WidthDefault);
    CHECK(json::lineWidthValue(RS2::Width12) == QJsonValue(0.53));
    CHECK(json::parseLineType(QJsonValue(QStringLiteral("continuous")), QStringLiteral("t"), true) == RS2::SolidLine);
    CHECK(json::parseLineType(QJsonValue(QStringLiteral("Dashed")), QStringLiteral("t"), true) == RS2::DashLine);
    const LC_ApiError badType = errorOf([] { json::parseLineType(QJsonValue(QStringLiteral("wiggly")), QStringLiteral("t"), true); });
    CHECK(badType.extra.value(QStringLiteral("allowed")).toArray().contains(QJsonValue(QStringLiteral("DASHED"))));
    CHECK(json::parseIdText(QStringLiteral("42"), QStringLiteral("id")) == 42ULL);
    CHECK(errorOf([] { json::parseIdText(QStringLiteral("delete"), QStringLiteral("id")); }).token == QStringLiteral("bad_param"));
    CHECK(errorOf([] { json::parseIds(QJsonArray{1, 2.5}, QStringLiteral("ids")); }).extra.value(QStringLiteral("param")).toString() ==
          QStringLiteral("ids[1]"));
}

TEST_CASE("Remote API: PATCH changes the geometry of every type and keeps the id", "[remote-api][entities]") {
    Drawing d;
    lc::test::SourceFonts fonts;
    addBlock(d.graphic, QStringLiteral("B1"));
    QJsonArray warnings;
    QString audit;
    struct Case {
        const char* spec;
        const char* patch;
        const char* field;
    };
    const Case cases[] = {
        {R"({"type":"line","start":[0,0],"end":[1,0]})", R"({"end":[2,2]})", "end"},
        {R"({"type":"circle","center":[0,0],"radius":1})", R"({"center":[3,3]})", "center"},
        {R"({"type":"arc","center":[0,0],"radius":1,"start_angle_deg":0,"end_angle_deg":90})", R"({"end_angle_deg":180})", "end_angle_deg"},
        {R"({"type":"ellipse","center":[0,0],"major_axis":[4,0],"ratio":0.5})", R"({"ratio":0.75})", "ratio"},
        {R"({"type":"point","position":[0,0]})", R"({"position":[1,1]})", "position"},
        {R"({"type":"polyline","vertices":[[0,0],[1,0]]})", R"({"vertices":[[0,0],[1,0,0.25],[1,1]]})", "vertices"},
        {R"({"type":"spline","control_points":[[0,0],[1,1],[2,0]],"degree":2})", R"({"control_points":[[0,0],[1,2],[2,0]]})", "control_points"},
        {R"({"type":"spline","control_points":[[0,0],[1,1],[2,0]],"degree":2})", R"({"fit_points":[[0,0],[1,1],[2,0]]})", "fit_points"},
        {R"({"type":"text","position":[0,0],"text":"a","height":1})", R"({"text":"b","height":2,"halign":"center"})", "text"},
        {R"({"type":"mtext","position":[0,0],"text":"a","height":1})", R"({"height":2,"attachment":"middle_center"})", "attachment"},
        {R"({"type":"dimension","p1":[0,0],"p2":[10,0],"line_point":[10,3]})", R"({"p2":[20,0],"line_point":[20,3]})", "p2"},
        {R"({"type":"hatch","boundary":[[[0,0],[1,0],[1,1]]]})", R"({"angle_deg":45})", "angle_deg"},
        {R"({"type":"insert","block":"B1","position":[0,0]})", R"({"scale":[2,3],"columns":2,"spacing":[5,0]})", "scale"},
    };
    for (const Case& c : cases) {
        const QJsonObject spec = object(c.spec);
        const QJsonObject patch = object(c.patch);
        const unsigned long long id = d.createOne(QStringLiteral(R"({"entities":[%1]})").arg(QString::fromUtf8(c.spec)).toUtf8().constData());
        const QJsonObject after = d.ops().update(id, LC_JsonReader(patch), warnings, audit).value(QStringLiteral("entity")).toObject();
        INFO(c.spec << " + " << c.patch << " -> " << compact(after).toStdString());
        CHECK(static_cast<unsigned long long>(after.value(QStringLiteral("id")).toInteger()) == id);
        CHECK(after.value(QStringLiteral("type")).toString() == spec.value(QStringLiteral("type")).toString());
        for (auto it = patch.constBegin(); it != patch.constEnd(); ++it) {
            CHECK(sameJson(it.value(), after.value(it.key())));
        }
        CHECK(after.contains(QLatin1String(c.field)));
    }
    // the dimension measures its new points
    const QJsonArray dims =
        d.ops().list(QMultiHash<QString, QString>{{QStringLiteral("type"), QStringLiteral("dimension")}}).value(QStringLiteral("entities")).toArray();
    REQUIRE(dims.size() == 1);
    CHECK(near(dims.at(0).toObject().value(QStringLiteral("measurement")).toDouble(), 20.0));
    // one live entity per case: every PATCH replaced its entity
    CHECK(d.liveCount() == static_cast<int>(std::size(cases)));
}
