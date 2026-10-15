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

#include "lc_remoteapientityfactory.h"

#include <cmath>
#include <vector>

#include <QJsonArray>

#include "lc_remoteapientityindex.h"
#include "lc_remoteapientityjson.h"
#include "lc_remoteapijson.h"
#include "lc_remoteapitypes.h"
#include "lc_splinepoints.h"
#include "rs_arc.h"
#include "rs_block.h"
#include "rs_circle.h"
#include "rs_dimaligned.h"
#include "rs_dimangular.h"
#include "rs_dimdiametric.h"
#include "rs_dimension.h"
#include "rs_dimlinear.h"
#include "rs_dimradial.h"
#include "rs_document.h"
#include "rs_ellipse.h"
#include "rs_font.h"
#include "rs_fontlist.h"
#include "rs_graphic.h"
#include "rs_hatch.h"
#include "rs_insert.h"
#include "rs_line.h"
#include "rs_math.h"
#include "rs_mtext.h"
#include "rs_point.h"
#include "rs_polyline.h"
#include "rs_spline.h"
#include "rs_text.h"

namespace {

namespace json = LC_RemoteApiEntityJson;

const QStringList& attributeFields() {
    static const QStringList fields{QStringLiteral("type"), QStringLiteral("layer"), QStringLiteral("color"),
                                    QStringLiteral("linetype"), QStringLiteral("lineweight")};
    return fields;
}

const QStringList& dimensionTypes() {
    static const QStringList types{QStringLiteral("linear"), QStringLiteral("aligned"), QStringLiteral("radial"),
                                   QStringLiteral("diametric"), QStringLiteral("angular")};
    return types;
}

QStringList geometryFieldsOf(const QString& type, const QString& dimType) {
    if (type == QLatin1String("line")) {
        return {QStringLiteral("start"), QStringLiteral("end")};
    }
    if (type == QLatin1String("circle")) {
        return {QStringLiteral("center"), QStringLiteral("radius")};
    }
    if (type == QLatin1String("arc")) {
        return {QStringLiteral("center"), QStringLiteral("radius"), QStringLiteral("start_angle_deg"),
                QStringLiteral("end_angle_deg"), QStringLiteral("reversed")};
    }
    if (type == QLatin1String("ellipse")) {
        return {QStringLiteral("center"), QStringLiteral("major_axis"), QStringLiteral("ratio"),
                QStringLiteral("start_angle_deg"), QStringLiteral("end_angle_deg"), QStringLiteral("reversed")};
    }
    if (type == QLatin1String("point")) {
        return {QStringLiteral("position")};
    }
    if (type == QLatin1String("polyline")) {
        return {QStringLiteral("vertices"), QStringLiteral("closed")};
    }
    if (type == QLatin1String("spline")) {
        return {QStringLiteral("control_points"), QStringLiteral("degree"), QStringLiteral("fit_points"), QStringLiteral("closed")};
    }
    if (type == QLatin1String("text")) {
        return {QStringLiteral("position"), QStringLiteral("text"), QStringLiteral("height"), QStringLiteral("angle_deg"),
                QStringLiteral("halign"), QStringLiteral("valign"), QStringLiteral("style"), QStringLiteral("width_factor"),
                QStringLiteral("second_point")};
    }
    if (type == QLatin1String("mtext")) {
        return {QStringLiteral("position"), QStringLiteral("text"), QStringLiteral("height"), QStringLiteral("width"),
                QStringLiteral("angle_deg"), QStringLiteral("attachment"), QStringLiteral("line_spacing"), QStringLiteral("style")};
    }
    if (type == QLatin1String("dimension")) {
        QStringList fields{QStringLiteral("dim_type"), QStringLiteral("text"), QStringLiteral("style")};
        const bool any = dimType.isEmpty();
        if (any || dimType == QLatin1String("linear")) {
            fields << QStringLiteral("p1") << QStringLiteral("p2") << QStringLiteral("line_point") << QStringLiteral("angle_deg");
        }
        if (dimType == QLatin1String("aligned")) {
            fields << QStringLiteral("p1") << QStringLiteral("p2") << QStringLiteral("line_point");
        }
        if (any || dimType == QLatin1String("radial")) {
            fields << QStringLiteral("center") << QStringLiteral("point") << QStringLiteral("leader_length");
        }
        if (dimType == QLatin1String("diametric")) {
            fields << QStringLiteral("point") << QStringLiteral("opposite_point");
        }
        if (any) {
            fields << QStringLiteral("opposite_point");
        }
        if (any || dimType == QLatin1String("angular")) {
            fields << QStringLiteral("line1") << QStringLiteral("line2") << QStringLiteral("arc_point");
        }
        return fields;
    }
    if (type == QLatin1String("hatch")) {
        return {QStringLiteral("pattern"), QStringLiteral("scale"), QStringLiteral("angle_deg"), QStringLiteral("boundary"),
                QStringLiteral("boundary_ids")};
    }
    if (type == QLatin1String("insert")) {
        return {QStringLiteral("block"), QStringLiteral("position"), QStringLiteral("scale"), QStringLiteral("angle_deg"),
                QStringLiteral("columns"), QStringLiteral("rows"), QStringLiteral("spacing")};
    }
    return {};
}

void checkFields(const QJsonObject& object, const QString& path, const QStringList& allowed, const QString& type) {
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!allowed.contains(it.key())) {
            const QString field = json::joinPath(path, it.key());
            json::throwUnknownField(field, QStringLiteral("'%1' is not a field of a %2 (fields: %3)")
                                               .arg(field, type, allowed.join(QStringLiteral(", "))));
        }
    }
}

/** 400 unknown_field of a PATCH body, with every field the entity accepts in "allowed". */
[[noreturn]] void throwPatchUnknownField(const QString& field, const QString& detail, const QStringList& geometry) {
    QStringList accepted = geometry;
    for (const QString& attribute : attributeFields()) {
        if (!accepted.contains(attribute)) {
            accepted << attribute;
        }
    }
    accepted.sort();
    throw LC_ApiError(400, QStringLiteral("unknown_field"), detail,
                      {{QStringLiteral("field"), field}, {QStringLiteral("allowed"), QJsonArray::fromStringList(accepted)}});
}

void checkPatchFields(const QJsonObject& fields, const QStringList& geometry, const QString& type) {
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        if (!geometry.contains(it.key())) {
            throwPatchUnknownField(it.key(),
                                   QStringLiteral("'%1' is not a field of a %2 (fields: %3; and layer, color, linetype, lineweight)")
                                       .arg(it.key(), type, geometry.join(QStringLiteral(", "))),
                                   geometry);
        }
    }
}

/** Knot vector scaled to [0, 1]: a reparameterization that leaves the curve as it is. */
std::vector<double> normalizedKnots(std::vector<double> knots) {
    if (knots.size() < 2) {
        return knots;
    }
    const double first = knots.front();
    const double span = knots.back() - first;
    for (double& knot : knots) {
        knot = span > 0.0 ? (knot - first) / span : 0.0;
    }
    return knots;
}

/**
 * Whether LibreCAD makes the same curve from the spline's control points,
 * degree and closed flag alone: no knot vector or weights of its own (imported
 * splines often have both), so rebuilding it loses nothing.
 */
bool rebuildable(const RS_Spline* spline) {
    for (const double weight : spline->getWeights()) {
        if (std::abs(weight - 1.0) > 1e-12) {
            return false;
        }
    }
    RS_Spline probe(nullptr, RS_SplineData(spline->getDegree(), spline->isClosed()));
    for (const RS_Vector& p : spline->getControlPoints()) {
        probe.addControlPoint(p);
    }
    probe.update();
    const std::vector<double> mine = normalizedKnots(spline->getKnotVector());
    const std::vector<double> standard = normalizedKnots(probe.getKnotVector());
    if (mine.size() != standard.size()) {
        return false;
    }
    for (size_t i = 0; i < mine.size(); ++i) {
        if (std::abs(mine[i] - standard[i]) > 1e-9) {
            return false;
        }
    }
    return true;
}

double radians(const double degrees) {
    return RS_Math::correctAngle(RS_Math::deg2rad(degrees));
}

bool sameAngle(const double degrees1, const double degrees2) {
    return std::abs(std::remainder(degrees2 - degrees1, 360.0)) < 1e-9;
}

double positive(const LC_JsonReader& r, const char* key, const QString& path) {
    const double v = r.num(key);
    if (!(v > 0.0)) {
        json::throwInvalidEntity(json::joinPath(path, QLatin1String(key)), QStringLiteral("must be > 0"));
    }
    return v;
}

double positive(const LC_JsonReader& r, const char* key, const double def, const QString& path) {
    const double v = r.num(key, def);
    if (!(v > 0.0)) {
        json::throwInvalidEntity(json::joinPath(path, QLatin1String(key)), QStringLiteral("must be > 0"));
    }
    return v;
}

void distinct(const RS_Vector& a, const RS_Vector& b, const QString& param, const QString& other) {
    if (a.distanceTo(b) < RS_TOLERANCE) {
        json::throwInvalidEntity(param, QStringLiteral("must differ from %1").arg(other));
    }
}

QString requireNonEmptyText(const LC_JsonReader& r, const QString& path) {
    const QString text = r.str("text");
    if (text.isEmpty()) {
        json::throwInvalidEntity(json::joinPath(path, QStringLiteral("text")), QStringLiteral("must not be empty"));
    }
    return text;
}

/** Text styles are fonts. A missing font falls back to "standard": that is a warning. */
QString checkFont(const QString& style, const QString& param, QStringList& warnings) {
    if (style.trimmed().isEmpty()) {
        json::throwInvalidEntity(param, QStringLiteral("must name a font"));
    }
    RS_Font* font = RS_FONTLIST->requestFont(style);
    if (font == nullptr) {
        json::throwInvalidEntity(param, QStringLiteral("no font '%1' is available (the font list may not be loaded)").arg(style));
    }
    if (font->getFileName().compare(style, Qt::CaseInsensitive) != 0) {
        warnings << QStringLiteral("%1: no font '%2'; LibreCAD draws the text with '%3'").arg(param, style, font->getFileName());
    }
    return style;
}

// Bulges below this are straight segments (see readVertices).
constexpr double MinBulge = 1e-8;

/** [x, y] or [x, y, bulge] vertices. */
void readVertices(const QJsonValue& value, const QString& path, const int minCount, QList<RS_Vector>& points, QList<double>& bulges) {
    if (!value.isArray()) {
        json::throwBadParam(path, QStringLiteral("'%1' must be an array of [x, y] or [x, y, bulge] vertices").arg(path));
    }
    const QJsonArray a = value.toArray();
    for (int i = 0; i < a.size(); ++i) {
        const QString vertexPath = json::indexPath(path, i);
        const QJsonArray v = a.at(i).toArray();
        if (!a.at(i).isArray() || v.size() < 2 || v.size() > 3) {
            json::throwBadParam(vertexPath, QStringLiteral("'%1' must be [x, y] or [x, y, bulge]").arg(vertexPath));
        }
        for (const QJsonValue& n : v) {
            if (!n.isDouble() || !std::isfinite(n.toDouble())) {
                json::throwBadParam(vertexPath, QStringLiteral("'%1' must hold finite numbers").arg(vertexPath));
            }
        }
        points.append(RS_Vector(v.at(0).toDouble(), v.at(1).toDouble()));
        double bulge = v.size() == 3 ? v.at(2).toDouble() : 0.0;
        // An arc this flat (sweep 4*atan(bulge) under 4e-8 rad) is a straight
        // segment within any tolerance; RS_Arc would take a sweep below its angle
        // tolerance for a FULL circle of radius chord/(4*bulge), which is huge.
        if (std::abs(bulge) < MinBulge) {
            bulge = 0.0;
        }
        bulges.append(bulge);
    }
    if (points.size() < minCount) {
        json::throwInvalidEntity(path, QStringLiteral("needs at least %1 vertices").arg(minCount));
    }
}

/** 1 | [sx, sy], neither component zero. */
RS_Vector readScale(const QJsonObject& object, const QString& key, const QString& path) {
    const QString param = json::joinPath(path, key);
    const QJsonValue value = object.value(key);
    RS_Vector scale(1.0, 1.0);
    if (value.isUndefined() || value.isNull()) {
        return scale;
    }
    if (value.isDouble()) {
        const double f = json::parseNumber(value, param);
        scale = RS_Vector(f, f);
    }
    else if (value.isArray()) {
        scale = json::parsePoint(value, param);
    }
    else {
        json::throwBadParam(param, QStringLiteral("'%1' must be a number or [sx, sy]").arg(param));
    }
    if (std::abs(scale.x) < RS_TOLERANCE || std::abs(scale.y) < RS_TOLERANCE) {
        json::throwInvalidEntity(param, QStringLiteral("must not be zero"));
    }
    return scale;
}

// Projection of p onto the line through origin with the unit direction dir.
RS_Vector project(const RS_Vector& p, const RS_Vector& origin, const RS_Vector& dir) {
    return origin + dir * RS_Vector::dotP(p - origin, dir);
}

struct TextParams {
    RS_Vector position;
    RS_Vector secondPoint;
    QString text;
    double height = 1.0;
    double angle = 0.0;
    double widthFactor = 1.0;
    RS_TextData::HAlign halign = RS_TextData::HALeft;
    RS_TextData::VAlign valign = RS_TextData::VABaseline;
    QString style;
};

TextParams readText(const LC_JsonReader& r, const QString& path, QStringList& warnings) {
    TextParams p;
    static const QStringList hAligns{QStringLiteral("left"), QStringLiteral("center"), QStringLiteral("right"),
                                     QStringLiteral("aligned"), QStringLiteral("middle"), QStringLiteral("fit")};
    static const QStringList vAligns{QStringLiteral("baseline"), QStringLiteral("bottom"), QStringLiteral("middle"), QStringLiteral("top")};
    p.halign = static_cast<RS_TextData::HAlign>(hAligns.indexOf(r.enumOf("halign", hAligns, QStringLiteral("left"))));
    p.valign = static_cast<RS_TextData::VAlign>(vAligns.indexOf(r.enumOf("valign", vAligns, QStringLiteral("baseline"))));
    p.position = r.point("position");
    p.text = requireNonEmptyText(r, path);
    // aligned text takes its height from position and second_point (checkDerivedText)
    p.height = p.halign == RS_TextData::HAAligned && !r.has("height") ? 1.0 : positive(r, "height", path);
    p.angle = radians(r.num("angle_deg", 0.0));
    p.widthFactor = positive(r, "width_factor", 1.0, path);
    p.style = checkFont(r.str("style", QStringLiteral("standard")), json::joinPath(path, QStringLiteral("style")), warnings);
    const std::optional<RS_Vector> second = r.optPoint("second_point");
    const bool needsSecond = p.halign == RS_TextData::HAAligned || p.halign == RS_TextData::HAFit;
    if (needsSecond) {
        if (!second.has_value()) {
            json::throwInvalidEntity(json::joinPath(path, QStringLiteral("second_point")),
                                     QStringLiteral("aligned and fit text need a second_point (the end of the baseline)"));
        }
        distinct(*second, p.position, json::joinPath(path, QStringLiteral("second_point")), QStringLiteral("position"));
    }
    p.secondPoint = second.value_or(p.position);
    return p;
}

/**
 * Aligned, fit and middle text get some fields from LibreCAD, not from the
 * body (RS_Text::update()): an aligned text's height and angle, and a fit
 * text's angle, follow from position and second_point; aligned and fit text
 * stretch between those two points, so width_factor is not used; and the three
 * sit on the baseline. A body that gives one of these fields a value the text
 * does not end with is refused (422 invalid_entity naming the field), never
 * silently changed; the values GET reports pass, so an echo works. given is
 * the body (POST: the spec; PATCH: the fields it names), keptWidth the
 * width_factor the entity had (1 for a new one).
 */
void checkDerivedText(const QJsonObject& given, const RS_Text& text, const QString& path, const double keptWidth) {
    const auto has = [&given](const char* key) {
        const QJsonValue value = given.value(QLatin1String(key));
        return !value.isUndefined() && !value.isNull();
    };
    const RS_TextData::HAlign halign = text.getHAlign();
    const bool aligned = halign == RS_TextData::HAAligned;
    const bool fit = halign == RS_TextData::HAFit;
    if (!aligned && !fit && halign != RS_TextData::HAMiddle) {
        return;
    }
    const QString kind = aligned ? QStringLiteral("aligned") : fit ? QStringLiteral("fit") : QStringLiteral("middle");
    const auto field = [&path](const char* key) {
        return json::joinPath(path, QLatin1String(key));
    };
    if (aligned && has("height")) {
        const double kept = text.getHeight();
        if (std::abs(given.value(QStringLiteral("height")).toDouble() - kept) > 1e-6 * std::max(1.0, std::abs(kept))) {
            json::throwInvalidEntity(field("height"),
                                     QStringLiteral("aligned text takes its height from the distance between position and "
                                                    "second_point (here %1): leave height out, or move second_point")
                                         .arg(kept, 0, 'g', 12));
        }
    }
    if ((aligned || fit) && has("angle_deg")) {
        const double kept = RS_Math::rad2deg(text.getAngle());
        if (std::abs(std::remainder(given.value(QStringLiteral("angle_deg")).toDouble() - kept, 360.0)) > 1e-6) {
            json::throwInvalidEntity(field("angle_deg"),
                                     QStringLiteral("%1 text takes its angle from position to second_point (here %2 degrees): "
                                                    "leave angle_deg out, or move second_point")
                                         .arg(kind)
                                         .arg(kept, 0, 'g', 12));
        }
    }
    if ((aligned || fit) && has("width_factor")) {
        const double wanted = given.value(QStringLiteral("width_factor")).toDouble();
        if (std::abs(wanted - keptWidth) > 1e-9 * std::max(1.0, std::abs(keptWidth))) {
            json::throwInvalidEntity(field("width_factor"),
                                     QStringLiteral("%1 text is stretched to fill position to second_point, so width_factor "
                                                    "is not used: leave it out")
                                         .arg(kind));
        }
    }
    if (has("valign") && given.value(QStringLiteral("valign")).toString() != QLatin1String("baseline")) {
        json::throwInvalidEntity(field("valign"),
                                 QStringLiteral("%1 text sits on its baseline: valign must be \"baseline\" (or left out)").arg(kind));
    }
}

struct MTextParams {
    RS_Vector position;
    QString text;
    double height = 1.0;
    double width = 0.0;
    double angle = 0.0;
    double lineSpacing = 1.0;
    RS_MTextData::VAlign valign = RS_MTextData::VATop;
    RS_MTextData::HAlign halign = RS_MTextData::HALeft;
    QString style;
};

MTextParams readMText(const LC_JsonReader& r, const QString& path, QStringList& warnings) {
    MTextParams p;
    p.position = r.point("position");
    p.text = requireNonEmptyText(r, path);
    p.height = positive(r, "height", path);
    p.width = r.num("width", 0.0);
    if (p.width < 0.0) {
        json::throwInvalidEntity(json::joinPath(path, QStringLiteral("width")), QStringLiteral("must be >= 0 (0: no wrapping)"));
    }
    p.angle = radians(r.num("angle_deg", 0.0));
    p.lineSpacing = positive(r, "line_spacing", 1.0, path);
    static const QStringList attachments{QStringLiteral("top_left"),    QStringLiteral("top_center"),    QStringLiteral("top_right"),
                                         QStringLiteral("middle_left"), QStringLiteral("middle_center"), QStringLiteral("middle_right"),
                                         QStringLiteral("bottom_left"), QStringLiteral("bottom_center"), QStringLiteral("bottom_right")};
    const int attachment = attachments.indexOf(r.enumOf("attachment", attachments, QStringLiteral("top_left")));
    p.valign = static_cast<RS_MTextData::VAlign>(attachment / 3);
    p.halign = static_cast<RS_MTextData::HAlign>(attachment % 3);
    p.style = checkFont(r.str("style", QStringLiteral("standard")), json::joinPath(path, QStringLiteral("style")), warnings);
    return p;
}

struct InsertParams {
    QString block;
    RS_Vector position;
    RS_Vector scale{1.0, 1.0};
    double angle = 0.0;
    int columns = 1;
    int rows = 1;
    RS_Vector spacing{0.0, 0.0};
};

InsertParams readInsert(const LC_JsonReader& r, const QString& path, RS_Graphic* graphic) {
    InsertParams p;
    p.block = r.str("block");
    if (graphic == nullptr || graphic->findBlock(p.block) == nullptr) {
        throw LC_ApiError(404, QStringLiteral("unknown_block"), QStringLiteral("no block named '%1'").arg(p.block),
                          {{QStringLiteral("name"), p.block}, {QStringLiteral("param"), json::joinPath(path, QStringLiteral("block"))}});
    }
    p.position = r.point("position");
    p.scale = readScale(r.raw(), QStringLiteral("scale"), path);
    p.angle = radians(r.num("angle_deg", 0.0));
    p.columns = r.integer("columns", 1, 1, 1000);
    p.rows = r.integer("rows", 1, 1, 1000);
    p.spacing = r.optPoint("spacing").value_or(RS_Vector(0.0, 0.0));
    return p;
}

struct HatchParams {
    QString pattern;
    bool solid = true;
    double scale = 1.0;
    double angle = 0.0;
};

HatchParams readHatch(const LC_JsonReader& r, const QString& path) {
    HatchParams p;
    p.pattern = r.str("pattern", QStringLiteral("SOLID")).trimmed();
    if (p.pattern.isEmpty()) {
        json::throwInvalidEntity(json::joinPath(path, QStringLiteral("pattern")), QStringLiteral("must name a hatch pattern"));
    }
    p.solid = p.pattern.compare(QLatin1String("SOLID"), Qt::CaseInsensitive) == 0;
    if (p.solid) {
        p.pattern = QStringLiteral("SOLID");
    }
    p.scale = positive(r, "scale", 1.0, path);
    p.angle = radians(r.num("angle_deg", 0.0));
    return p;
}

/**
 * validate(), update() and the update error of a hatch with its loops. A PATCH
 * passes the error the hatch had before (read from a file, it may name a
 * pattern LibreCAD has no definition of): that one is no error of the request,
 * unless the request changed what it is about.
 */
void finishHatch(RS_Hatch* hatch, const QString& path, const RS_Hatch::RS_HatchError before = RS_Hatch::HATCH_OK,
                 const bool patternChanged = true, const bool scaleChanged = true) {
    const bool validated = hatch->validate();
    if (validated) {
        hatch->update();
    }
    const RS_Hatch::RS_HatchError error = validated ? hatch->getUpdateError() : RS_Hatch::HATCH_INVALID_CONTOUR;
    if (error != RS_Hatch::HATCH_OK && error == before) {
        bool caused = false;
        switch (error) {
            case RS_Hatch::HATCH_PATTERN_NOT_FOUND:
                caused = patternChanged;
                break;
            case RS_Hatch::HATCH_TOO_SMALL:
            case RS_Hatch::HATCH_AREA_TOO_BIG:
                caused = patternChanged || scaleChanged;
                break;
            default:
                break;
        }
        if (!caused) {
            return; // as it was
        }
    }
    if (!validated) {
        throw LC_ApiError(422, QStringLiteral("invalid_hatch_boundary"), QStringLiteral("the hatch boundary is not a valid closed contour"),
                          {{QStringLiteral("param"), path}});
    }
    switch (error) {
        case RS_Hatch::HATCH_OK:
            return;
        case RS_Hatch::HATCH_PATTERN_NOT_FOUND:
            json::throwInvalidEntity(json::joinPath(path, QStringLiteral("pattern")),
                                     QStringLiteral("unknown hatch pattern '%1'").arg(hatch->getPattern()));
        case RS_Hatch::HATCH_TOO_SMALL:
            throw LC_ApiError(422, QStringLiteral("invalid_hatch_boundary"), QStringLiteral("the contour or the pattern is too small"),
                              {{QStringLiteral("param"), path}});
        case RS_Hatch::HATCH_AREA_TOO_BIG:
            throw LC_ApiError(422, QStringLiteral("invalid_hatch_boundary"), QStringLiteral("the contour is too big for the pattern scale"),
                              {{QStringLiteral("param"), path}});
        case RS_Hatch::HATCH_INVALID_CONTOUR:
        default:
            throw LC_ApiError(422, QStringLiteral("invalid_hatch_boundary"), QStringLiteral("the hatch boundary is not a valid closed contour"),
                              {{QStringLiteral("param"), path}});
    }
}

} // namespace

LC_RemoteApiEntityFactory::LC_RemoteApiEntityFactory(RS_Document* document, const LC_RemoteApiEntityIndex* index)
    : m_document(document)
    , m_graphic(document != nullptr ? document->getGraphic() : nullptr)
    , m_index(index) {
}

void LC_RemoteApiEntityFactory::transplantIdentity(const RS_Entity* original, RS_Entity* fresh) {
    RS_Layer* layer = original->getLayer(false);
    const RS_Pen pen = original->getPen(false);
    static_cast<RS_Entity&>(*fresh) = static_cast<const RS_Entity&>(*original);
    fresh->setLayer(layer);
    fresh->setPen(pen);
    fresh->calculateBorders();
}

QStringList LC_RemoteApiEntityFactory::specFields(const QString& typeName, const QString& dimType) {
    return attributeFields() + geometryFieldsOf(typeName, dimType);
}

std::unique_ptr<RS_Entity> LC_RemoteApiEntityFactory::create(const QJsonObject& spec, const QString& path, QStringList& warnings) const {
    return createEntity(spec, path, warnings, false);
}

std::unique_ptr<RS_Entity> LC_RemoteApiEntityFactory::createEntity(const QJsonObject& spec, const QString& path, QStringList& warnings,
                                                                   const bool keepAngularOrder) const {
    const QString typePath = json::joinPath(path, QStringLiteral("type"));
    const QJsonValue typeValue = spec.value(QStringLiteral("type"));
    const QStringList creatable = json::creatableTypeNames();
    if (typeValue.isUndefined()) {
        json::throwMissingParam(typePath);
    }
    if (!typeValue.isString()) {
        json::throwBadParam(typePath, QStringLiteral("'%1' must be an entity type name").arg(typePath), QJsonArray::fromStringList(creatable));
    }
    const QString type = typeValue.toString();
    if (!creatable.contains(type)) {
        if (json::allTypeNames().contains(type)) {
            json::throwInvalidEntity(typePath, QStringLiteral("%1 entities are read-only in this version").arg(type));
        }
        json::throwBadParam(typePath, QStringLiteral("unknown entity type '%1'").arg(type), QJsonArray::fromStringList(creatable));
    }

    const LC_JsonReader r(spec, path);
    QString dimType;
    if (type == QLatin1String("dimension")) {
        dimType = r.enumOf("dim_type", dimensionTypes(), QStringLiteral("linear"));
    }
    checkFields(spec, path, specFields(type, dimType), dimType.isEmpty() ? type : dimType + QStringLiteral(" dimension"));

    const auto field = [&path](const char* key) {
        return json::joinPath(path, QLatin1String(key));
    };

    if (type == QLatin1String("line")) {
        const RS_Vector start = r.point("start");
        const RS_Vector end = r.point("end");
        distinct(end, start, field("end"), QStringLiteral("start"));
        return std::make_unique<RS_Line>(m_document, RS_LineData(start, end));
    }
    if (type == QLatin1String("circle")) {
        const RS_Vector center = r.point("center");
        const double radius = positive(r, "radius", path);
        return std::make_unique<RS_Circle>(m_document, RS_CircleData(center, radius));
    }
    if (type == QLatin1String("arc")) {
        const RS_Vector center = r.point("center");
        const double radius = positive(r, "radius", path);
        const double start = r.num("start_angle_deg");
        const double end = r.num("end_angle_deg");
        if (sameAngle(start, end)) {
            json::throwInvalidEntity(field("end_angle_deg"), QStringLiteral("must differ from start_angle_deg (a full circle is a circle)"));
        }
        const bool reversed = r.flag("reversed", false);
        return std::make_unique<RS_Arc>(m_document, RS_ArcData(center, radius, radians(start), radians(end), reversed));
    }
    if (type == QLatin1String("ellipse")) {
        const RS_Vector center = r.point("center");
        const RS_Vector majorAxis = r.point("major_axis");
        if (majorAxis.magnitude() < RS_TOLERANCE) {
            json::throwInvalidEntity(field("major_axis"), QStringLiteral("must not be zero (it is the vector from the center to the end of the major axis)"));
        }
        const double ratio = r.num("ratio");
        if (!(ratio > 0.0 && ratio <= 1.0)) {
            json::throwInvalidEntity(field("ratio"), QStringLiteral("must be in (0, 1] (minor axis / major axis)"));
        }
        const double start = r.num("start_angle_deg", 0.0);
        const double end = r.num("end_angle_deg", 360.0);
        const bool full = sameAngle(start, end);
        const bool reversed = r.flag("reversed", false);
        const RS_EllipseData data{center, majorAxis, ratio, full ? 0.0 : radians(start), full ? 0.0 : radians(end), reversed};
        return std::make_unique<RS_Ellipse>(m_document, data);
    }
    if (type == QLatin1String("point")) {
        return std::make_unique<RS_Point>(m_document, RS_PointData(r.point("position")));
    }
    if (type == QLatin1String("polyline")) {
        QList<RS_Vector> points;
        QList<double> bulges;
        readVertices(spec.value(QStringLiteral("vertices")), field("vertices"), 2, points, bulges);
        const bool closed = r.flag("closed", false);
        RS_PolylineData data;
        if (closed) {
            data.setFlag(RS2::FlagClosed);
        }
        auto polyline = std::make_unique<RS_Polyline>(m_document, data);
        for (int i = 0; i < points.size(); ++i) {
            polyline->addVertex(points.at(i), bulges.at(i));
        }
        return polyline;
    }
    if (type == QLatin1String("spline")) {
        const bool hasControl = r.has("control_points");
        const bool hasFit = r.has("fit_points");
        if (hasControl == hasFit) {
            json::throwInvalidEntity(field("control_points"), QStringLiteral("give either control_points or fit_points"));
        }
        const bool closed = r.flag("closed", false);
        if (hasControl) {
            const int degree = r.integer("degree", 3, 1, 3);
            const QList<RS_Vector> points = json::parsePoints(spec.value(QStringLiteral("control_points")), field("control_points"), degree + 1);
            auto spline = std::make_unique<RS_Spline>(m_document, RS_SplineData(degree, closed));
            for (const RS_Vector& p : points) {
                spline->addControlPoint(p);
            }
            spline->update();
            return spline;
        }
        if (r.has("degree")) {
            json::throwInvalidEntity(field("degree"), QStringLiteral("applies to control_points only; fit_points splines are quadratic"));
        }
        const QList<RS_Vector> points = json::parsePoints(spec.value(QStringLiteral("fit_points")), field("fit_points"), closed ? 3 : 2);
        LC_SplinePointsData data(closed, false);
        data.splinePoints.assign(points.cbegin(), points.cend());
        auto spline = std::make_unique<LC_SplinePoints>(m_document, data);
        spline->update();
        return spline;
    }
    if (type == QLatin1String("text")) {
        const TextParams p = readText(r, path, warnings);
        const RS_TextData data(p.position, p.secondPoint, p.height, p.widthFactor, p.valign, p.halign, RS_TextData::None, p.text, p.style,
                               p.angle, RS2::Update);
        auto text = std::make_unique<RS_Text>(m_document, data);
        checkDerivedText(spec, *text, path, 1.0);
        return text;
    }
    if (type == QLatin1String("mtext")) {
        const MTextParams p = readMText(r, path, warnings);
        const RS_MTextData data(p.position, p.height, p.width, p.valign, p.halign, RS_MTextData::LeftToRight, RS_MTextData::AtLeast,
                                p.lineSpacing, p.text, p.style, p.angle, RS2::Update);
        return std::make_unique<RS_MText>(m_document, data);
    }
    if (type == QLatin1String("dimension")) {
        const QString text = r.str("text", QString());
        const QString defaultStyle = m_graphic != nullptr ? m_graphic->getDefaultDimStyleName() : QStringLiteral("Standard");
        const QString style = r.str("style", defaultStyle);
        if (style != defaultStyle && m_graphic != nullptr && m_graphic->getDimStyleByName(style) == nullptr) {
            warnings << QStringLiteral("%1: no dimension style '%2'; LibreCAD uses the drawing's default").arg(field("style"), style);
        }
        RS_DimensionData data(RS_Vector(false), RS_Vector(false), RS_MTextData::VAMiddle, RS_MTextData::HACenter, RS_MTextData::Exact,
                              1.0, text, style, 0.0, 0.0, true, nullptr, false, false);
        std::unique_ptr<RS_Dimension> dim;
        if (dimType == QLatin1String("linear") || dimType == QLatin1String("aligned")) {
            const RS_Vector p1 = r.point("p1");
            const RS_Vector p2 = r.point("p2");
            const RS_Vector linePoint = r.point("line_point");
            distinct(p2, p1, field("p2"), QStringLiteral("p1"));
            if (dimType == QLatin1String("linear")) {
                const double angle = radians(r.num("angle_deg", 0.0));
                // the dimension line runs through line_point in the measuring direction
                data.definitionPoint = project(linePoint, p2, RS_Vector::polar(1.0, angle + M_PI_2));
                dim = std::make_unique<RS_DimLinear>(m_document, data, RS_DimLinearData(p1, p2, angle, 0.0));
            }
            else {
                data.definitionPoint = project(linePoint, p2, RS_Vector::polar(1.0, p1.angleTo(p2) + M_PI_2));
                dim = std::make_unique<RS_DimAligned>(m_document, data, RS_DimAlignedData(p1, p2));
            }
        }
        else if (dimType == QLatin1String("radial")) {
            const RS_Vector center = r.point("center");
            const RS_Vector point = r.point("point");
            distinct(point, center, field("point"), QStringLiteral("center"));
            const double leader = r.num("leader_length", 0.0);
            if (leader < 0.0) {
                json::throwInvalidEntity(field("leader_length"), QStringLiteral("must be >= 0"));
            }
            data.definitionPoint = center;
            dim = std::make_unique<RS_DimRadial>(m_document, data, RS_DimRadialData(point, leader));
        }
        else if (dimType == QLatin1String("diametric")) {
            const RS_Vector point = r.point("point");
            const RS_Vector opposite = r.point("opposite_point");
            distinct(opposite, point, field("opposite_point"), QStringLiteral("point"));
            data.definitionPoint = opposite;
            dim = std::make_unique<RS_DimDiametric>(m_document, data, RS_DimDiametricData(point, 0.0));
        }
        else {
            const QList<RS_Vector> line1 = json::parsePoints(spec.value(QStringLiteral("line1")), field("line1"), 2);
            const QList<RS_Vector> line2 = json::parsePoints(spec.value(QStringLiteral("line2")), field("line2"), 2);
            const RS_Vector arcPoint = r.point("arc_point");
            if (line1.size() != 2) {
                json::throwInvalidEntity(field("line1"), QStringLiteral("must be two points [P, P]"));
            }
            if (line2.size() != 2) {
                json::throwInvalidEntity(field("line2"), QStringLiteral("must be two points [P, P]"));
            }
            distinct(line1.at(1), line1.at(0), field("line1"), QStringLiteral("its first point"));
            distinct(line2.at(1), line2.at(0), field("line2"), QStringLiteral("its first point"));
            const RS_Vector d1 = (line1.at(1) - line1.at(0)).normalized();
            const RS_Vector d2 = (line2.at(1) - line2.at(0)).normalized();
            const double cross = RS_Vector::crossP(d1, d2).z;
            if (std::abs(cross) < 1e-9) {
                json::throwInvalidEntity(field("line2"), QStringLiteral("is parallel to line1: an angle needs two crossing lines"));
            }
            // the vertex: line1.at(0) + t * d1 on line 2
            const double t = RS_Vector::crossP(line2.at(0) - line1.at(0), d2).z / cross;
            const RS_Vector center = line1.at(0) + d1 * t;
            distinct(arcPoint, center, field("arc_point"), QStringLiteral("the vertex of the angle"));
            if (keepAngularOrder) {
                // the points as GET reports them: line1 = [P2, P1], line2 = [P, P3]
                data.definitionPoint = line2.at(0);
                dim = std::make_unique<RS_DimAngular>(m_document, data,
                                                      RS_DimAngularData(line1.at(1), line1.at(0), line2.at(1), arcPoint));
                dim->update();
                return dim;
            }
            // The four rays from the vertex alternate between the lines; the angle
            // measured is the sector holding arc_point, counterclockwise from the
            // ray before it to the ray after it.
            const double rays[4] = {d1.angle(), RS_Math::correctAngle(d1.angle() + M_PI), d2.angle(),
                                    RS_Math::correctAngle(d2.angle() + M_PI)};
            const double arcAngle = center.angleTo(arcPoint);
            int startRay = 0;
            int endRay = 0;
            double toStart = 4 * M_PI;
            double toEnd = 4 * M_PI;
            for (int i = 0; i < 4; ++i) {
                const double before = RS_Math::correctAngle(arcAngle - rays[i]);
                const double after = RS_Math::correctAngle(rays[i] - arcAngle);
                if (before < toStart) {
                    toStart = before;
                    startRay = i;
                }
                if (after < toEnd) {
                    toEnd = after;
                    endRay = i;
                }
            }
            const QList<RS_Vector>& startLine = startRay < 2 ? line1 : line2;
            const QList<RS_Vector>& endLine = endRay < 2 ? line1 : line2;
            const RS_Vector startDir = RS_Vector::polar(1.0, rays[startRay]);
            const RS_Vector endDir = RS_Vector::polar(1.0, rays[endRay]);
            // the far point along each ray first, as RS_ActionDimAngular orders them
            const auto alongRay = [&center](const RS_Vector& p, const RS_Vector& dir) {
                return RS_Vector::dotP(p - center, dir);
            };
            const bool startFirstFar = alongRay(startLine.at(0), startDir) >= alongRay(startLine.at(1), startDir);
            const bool endFirstFar = alongRay(endLine.at(0), endDir) >= alongRay(endLine.at(1), endDir);
            const RS_Vector startFar = startFirstFar ? startLine.at(0) : startLine.at(1);
            const RS_Vector startNear = startFirstFar ? startLine.at(1) : startLine.at(0);
            const RS_Vector endFar = endFirstFar ? endLine.at(0) : endLine.at(1);
            const RS_Vector endNear = endFirstFar ? endLine.at(1) : endLine.at(0);
            data.definitionPoint = endFar;
            dim = std::make_unique<RS_DimAngular>(m_document, data, RS_DimAngularData(startNear, startFar, endNear, arcPoint));
        }
        dim->update();
        return dim;
    }
    if (type == QLatin1String("hatch")) {
        const HatchParams p = readHatch(r, path);
        const bool hasBoundary = r.has("boundary");
        const bool hasIds = r.has("boundary_ids");
        if (hasBoundary == hasIds) {
            json::throwInvalidEntity(field("boundary"), QStringLiteral("give either boundary (loops of vertices) or boundary_ids (closed entities)"));
        }
        auto hatch = std::make_unique<RS_Hatch>(m_document, RS_HatchData(p.solid, p.scale, p.angle, p.pattern));
        if (hasBoundary) {
            const QJsonValue boundary = spec.value(QStringLiteral("boundary"));
            const QJsonArray loops = boundary.toArray();
            if (!boundary.isArray() || loops.isEmpty()) {
                json::throwBadParam(field("boundary"), QStringLiteral("'%1' must be a non-empty array of loops").arg(field("boundary")));
            }
            for (int i = 0; i < loops.size(); ++i) {
                const QString loopPath = json::indexPath(field("boundary"), i);
                QJsonValue vertices = loops.at(i);
                QString verticesPath = loopPath;
                if (loops.at(i).isObject()) {
                    const QJsonObject loopObject = loops.at(i).toObject();
                    checkFields(loopObject, loopPath, {QStringLiteral("vertices"), QStringLiteral("closed")}, QStringLiteral("hatch loop"));
                    const QJsonValue closed = loopObject.value(QStringLiteral("closed"));
                    if (!closed.isUndefined() && closed != QJsonValue(true)) {
                        json::throwInvalidEntity(json::joinPath(loopPath, QStringLiteral("closed")), QStringLiteral("a hatch loop is always closed"));
                    }
                    vertices = loopObject.value(QStringLiteral("vertices"));
                    verticesPath = json::joinPath(loopPath, QStringLiteral("vertices"));
                    if (vertices.isUndefined()) {
                        json::throwMissingParam(verticesPath);
                    }
                }
                QList<RS_Vector> points;
                QList<double> bulges;
                readVertices(vertices, verticesPath, 2, points, bulges);
                RS_PolylineData data;
                data.setFlag(RS2::FlagClosed);
                RS_Polyline outline(nullptr, data);
                for (int k = 0; k < points.size(); ++k) {
                    outline.addVertex(points.at(k), bulges.at(k));
                }
                auto* loop = new RS_EntityContainer(hatch.get());
                loop->setPen(RS_Pen(RS2::FlagInvalid));
                for (const RS_Entity* segment : outline) {
                    if (segment == nullptr || !segment->isAtomic()) {
                        continue;
                    }
                    RS_Entity* clone = segment->clone();
                    clone->setPen(RS_Pen(RS2::FlagInvalid));
                    clone->reparent(loop);
                    loop->addEntity(clone);
                }
                hatch->addEntity(loop);
            }
        }
        else {
            const QList<unsigned long long> ids = json::parseIds(spec.value(QStringLiteral("boundary_ids")), field("boundary_ids"));
            if (m_index == nullptr) {
                throw LC_ApiError(500, QStringLiteral("internal"), QStringLiteral("no entity index for boundary_ids"));
            }
            const QList<RS_Entity*> entities = m_index->requireAll(ids);
            // one loop holding every boundary entity, as RS_ActionDrawHatch makes it
            auto* loop = new RS_EntityContainer(hatch.get());
            loop->setPen(RS_Pen(RS2::FlagInvalid));
            for (const RS_Entity* e : entities) {
                RS_Entity* clone = e->clone();
                clone->setPen(RS_Pen(RS2::FlagInvalid));
                clone->reparent(loop);
                loop->addEntity(clone);
            }
            hatch->addEntity(loop);
        }
        finishHatch(hatch.get(), path);
        return hatch;
    }
    // insert
    const InsertParams p = readInsert(r, path, m_graphic);
    const RS_InsertData data(p.block, p.position, p.scale, p.angle, p.columns, p.rows, p.spacing, nullptr, RS2::Update);
    return std::make_unique<RS_Insert>(m_document, data);
}

std::unique_ptr<RS_Entity> LC_RemoteApiEntityFactory::patch(RS_Entity* original, const QJsonObject& body, QStringList& warnings) const {
    const QString type = json::typeName(original);
    const QJsonValue typeValue = body.value(QStringLiteral("type"));
    if (!typeValue.isUndefined() && typeValue.toString() != type) {
        json::throwBadParam(QStringLiteral("type"),
                            QStringLiteral("the type of an entity cannot change (it is a %1); delete it and create a new one").arg(type));
    }
    QJsonObject fields = body;
    for (const QString& key : attributeFields()) {
        fields.remove(key);
    }
    if (fields.isEmpty()) {
        return std::unique_ptr<RS_Entity>(original->clone());
    }
    const QString dimType = json::dimensionTypeName(original->rtti());
    const bool readOnly = !json::creatableTypeNames().contains(type) ||
                          (type == QLatin1String("dimension") && !dimensionTypes().contains(dimType));
    if (readOnly) {
        throwPatchUnknownField(fields.constBegin().key(),
                               QStringLiteral("this %1 is read-only: only layer, color, linetype and lineweight can change")
                                   .arg(dimType.isEmpty() ? type : dimType + QStringLiteral(" dimension")),
                               {});
    }
    if (type == QLatin1String("dimension")) {
        const QJsonValue requested = fields.value(QStringLiteral("dim_type"));
        if (!requested.isUndefined() && requested.toString() != dimType) {
            json::throwBadParam(QStringLiteral("dim_type"),
                                QStringLiteral("the dim_type of a dimension cannot change (it is %1); delete it and create a new one").arg(dimType));
        }
    }
    checkPatchFields(fields, geometryFieldsOf(type, dimType), dimType.isEmpty() ? type : dimType + QStringLiteral(" dimension"));
    if (original->rtti() == RS2::EntityPolyline && json::hasEllipticSegments(static_cast<RS_Polyline*>(original))) {
        // bulges describe circular arcs: a rebuild would turn the elliptic segments into them
        json::throwInvalidEntity(QStringLiteral("vertices"),
                                 QStringLiteral("this polyline has elliptic segments (it was scaled by different x and y "
                                                "factors), which vertices with bulges cannot describe; delete it and draw it again"));
    }
    if (type == QLatin1String("hatch")) {
        for (const char* key : {"boundary", "boundary_ids"}) {
            if (fields.contains(QLatin1String(key))) {
                json::throwBadParam(QLatin1String(key),
                                    QStringLiteral("the boundary of a hatch cannot change; delete the hatch and create a new one"));
            }
        }
    }

    QJsonObject merged = json::geometryFields(original);
    if (type == QLatin1String("spline")) {
        if (fields.contains(QStringLiteral("control_points"))) {
            merged.remove(QStringLiteral("fit_points"));
        }
        if (fields.contains(QStringLiteral("fit_points"))) {
            merged.remove(QStringLiteral("control_points"));
            merged.remove(QStringLiteral("degree"));
        }
    }
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        merged.insert(it.key(), it.value());
    }
    merged.insert(QStringLiteral("type"), type);
    const LC_JsonReader r(merged, QString());

    const auto given = [&fields](const char* key) {
        return fields.contains(QLatin1String(key));
    };
    if (type == QLatin1String("text")) {
        const TextParams p = readText(r, QString(), warnings);
        std::unique_ptr<RS_Entity> clone(original->clone());
        auto* text = static_cast<RS_Text*>(clone.get());
        // Alignment first: setText() lays the text out with the alignment it
        // finds, and a layout with a non-aligned halign overwrites the second
        // point. The second point is set again right before the final layout.
        text->setHAlign(p.halign);
        text->setVAlign(p.valign);
        text->setInsertionPoint(p.position);
        text->setHeight(p.height);
        if (given("angle_deg")) {
            text->setAngle(p.angle);
        }
        text->setWidthRel(p.widthFactor);
        text->setStyle(p.style);
        text->setSecondPoint(p.secondPoint);
        text->setText(p.text);
        text->setSecondPoint(p.secondPoint);
        text->update();
        checkDerivedText(fields, *text, QString(), static_cast<const RS_Text*>(original)->getWidthRel());
        return clone;
    }
    if (type == QLatin1String("mtext")) {
        const MTextParams p = readMText(r, QString(), warnings);
        std::unique_ptr<RS_Entity> clone(original->clone());
        auto* text = static_cast<RS_MText*>(clone.get());
        text->setInsertionPoint(p.position);
        text->setText(p.text);
        text->setHeight(p.height);
        text->setWidth(p.width);
        if (given("angle_deg")) {
            text->setAngle(p.angle);
        }
        text->setLineSpacingFactor(p.lineSpacing);
        text->setHAlign(p.halign);
        text->setVAlign(p.valign);
        text->setStyle(p.style);
        text->update();
        return clone;
    }
    if (type == QLatin1String("insert")) {
        const InsertParams p = readInsert(r, QString(), m_graphic);
        std::unique_ptr<RS_Entity> clone(original->clone());
        auto* insert = static_cast<RS_Insert*>(clone.get());
        insert->setName(p.block);
        insert->setInsertionPoint(p.position);
        insert->setScale(p.scale);
        if (given("angle_deg")) {
            insert->setAngle(p.angle);
        }
        insert->setCols(p.columns);
        insert->setRows(p.rows);
        insert->setSpacing(p.spacing);
        insert->update();
        return clone;
    }
    if (type == QLatin1String("hatch")) {
        // Only what the body names is read and checked: a hatch read from a file
        // keeps a pattern LibreCAD has no definition of, or the scale 0 of a solid
        // fill, while another field changes.
        std::unique_ptr<RS_Entity> clone(original->clone());
        auto* hatch = static_cast<RS_Hatch*>(clone.get());
        const RS_Hatch::RS_HatchError before = hatch->getUpdateError();
        bool patternChanged = false;
        if (given("pattern")) {
            QString pattern = r.str("pattern").trimmed();
            if (pattern.isEmpty()) {
                json::throwInvalidEntity(QStringLiteral("pattern"), QStringLiteral("must name a hatch pattern"));
            }
            const bool solid = pattern.compare(QLatin1String("SOLID"), Qt::CaseInsensitive) == 0;
            if (solid) {
                pattern = QStringLiteral("SOLID");
            }
            patternChanged = solid != hatch->isSolid() || (!solid && pattern.compare(hatch->getPattern(), Qt::CaseInsensitive) != 0);
            if (patternChanged) { // the same pattern keeps its spelling
                hatch->setSolid(solid);
                hatch->setPattern(pattern);
            }
        }
        // the scale as GET reports it (0 for a solid fill from some files) is no change
        const bool scaleChanged = given("scale") && r.num("scale") != hatch->getScale();
        if (scaleChanged) {
            hatch->setScale(positive(r, "scale", 1.0, QString()));
        }
        else if (patternChanged && !hatch->isSolid() && !(hatch->getScale() > 0.0)) {
            json::throwInvalidEntity(QStringLiteral("scale"),
                                     QStringLiteral("this hatch has scale %1 (a solid fill ignores it): give a scale > 0 with the pattern")
                                         .arg(hatch->getScale()));
        }
        if (given("angle_deg")) {
            hatch->setAngle(radians(r.num("angle_deg")));
        }
        finishHatch(hatch, QString(), before, patternChanged, scaleChanged);
        return clone;
    }
    if (type == QLatin1String("dimension")) {
        bool textOnly = true;
        for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
            textOnly = textOnly && (it.key() == QLatin1String("text") || it.key() == QLatin1String("style") ||
                                    it.key() == QLatin1String("dim_type"));
        }
        if (textOnly) {
            // the geometry stays exactly as it is (an imported angular dimension
            // keeps the angle it shows): only the text and the style change
            std::unique_ptr<RS_Entity> clone(original->clone());
            auto* dim = static_cast<RS_Dimension*>(clone.get());
            if (given("text")) {
                dim->setLabel(r.str("text", QString()));
            }
            if (given("style")) {
                const QString style = r.str("style");
                if (m_graphic != nullptr && style != m_graphic->getDefaultDimStyleName() &&
                    m_graphic->getDimStyleByName(style) == nullptr) {
                    warnings << QStringLiteral("style: no dimension style '%1'; LibreCAD uses the drawing's default").arg(style);
                }
                dim->setStyle(style);
            }
            dim->update();
            return clone;
        }
    }
    if (original->rtti() == RS2::EntitySpline && !given("fit_points")) {
        const auto* before = static_cast<const RS_Spline*>(original);
        const QList<RS_Vector> points = json::parsePoints(merged.value(QStringLiteral("control_points")),
                                                          QStringLiteral("control_points"), 2);
        const int degree = r.integer("degree", 3, 1, 3);
        const bool closed = r.flag("closed", false);
        const bool sameShape = static_cast<size_t>(points.size()) == before->getControlPoints().size() &&
                               degree == before->getDegree() && closed == before->isClosed();
        if (sameShape) {
            // new control points on the same structure: the knots and weights stay
            std::unique_ptr<RS_Entity> clone(original->clone());
            auto* spline = static_cast<RS_Spline*>(clone.get());
            for (qsizetype i = 0; i < points.size(); ++i) {
                spline->setControlPoint(static_cast<size_t>(i), points.at(i));
            }
            spline->update();
            spline->calculateBorders();
            return clone;
        }
        if (!rebuildable(before)) {
            json::throwInvalidEntity(given("degree") ? QStringLiteral("degree")
                                     : given("closed") ? QStringLiteral("closed")
                                                       : QStringLiteral("control_points"),
                                     QStringLiteral("this spline has its own knot vector or weights (it was read from a file), "
                                                    "which a new degree, closed flag or number of control points would lose; "
                                                    "move its control points only, or delete it and create a new spline"));
        }
    }

    // An angular dimension keeps the order of its points (file semantics, as an
    // imported one has them) only while its definition is the one GET reports:
    // a new line or arc_point measures the sector that holds arc_point, as POST does.
    bool keepAngularOrder = false;
    if (original->rtti() == RS2::EntityDimAngular) {
        const QJsonObject before = json::geometryFields(original);
        keepAngularOrder = true;
        for (const char* key : {"line1", "line2", "arc_point"}) {
            keepAngularOrder = keepAngularOrder && merged.value(QLatin1String(key)) == before.value(QLatin1String(key));
        }
    }
    std::unique_ptr<RS_Entity> fresh = createEntity(merged, QString(), warnings, keepAngularOrder);
    transplantIdentity(original, fresh.get());
    // Angles the body does not name keep their exact value: the merged degrees
    // went through a conversion and back.
    if (original->rtti() == RS2::EntityArc && fresh->rtti() == RS2::EntityArc) {
        const auto* before = static_cast<const RS_Arc*>(original);
        auto* after = static_cast<RS_Arc*>(fresh.get());
        if (!given("start_angle_deg")) {
            after->setAngle1(before->getAngle1());
        }
        if (!given("end_angle_deg")) {
            after->setAngle2(before->getAngle2());
        }
    }
    else if (original->rtti() == RS2::EntityEllipse && fresh->rtti() == RS2::EntityEllipse) {
        const auto* before = static_cast<const RS_Ellipse*>(original);
        auto* after = static_cast<RS_Ellipse*>(fresh.get());
        if (!json::isFullEllipse(before) && !json::isFullEllipse(after)) {
            if (!given("start_angle_deg")) {
                after->setAngle1(before->getAngle1());
            }
            if (!given("end_angle_deg")) {
                after->setAngle2(before->getAngle2());
            }
        }
    }
    else if (original->rtti() == RS2::EntityDimLinear && fresh->rtti() == RS2::EntityDimLinear && !given("angle_deg")) {
        static_cast<RS_DimLinear*>(fresh.get())->setAngle(static_cast<const RS_DimLinear*>(original)->getAngle());
    }
    if (RS2::isDimensionalEntity(original->rtti())) {
        const auto* before = static_cast<const RS_Dimension*>(original);
        auto* after = static_cast<RS_Dimension*>(fresh.get());
        after->setDimStyleOverride(before->getDimStyleOverride());
        after->setFlipArrow1(before->isFlipArrow1());
        after->setFlipArrow2(before->isFlipArrow2());
    }
    fresh->update();
    fresh->calculateBorders();
    return fresh;
}
