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

#include "lc_remoteapientityjson.h"

#include <cmath>
#include <utility>

#include <QColor>
#include <QRegularExpression>

#include "lc_colornumbers.h"
#include "lc_linetypenames.h"
#include "lc_remoteapijson.h"
#include "lc_remoteapitypes.h"
#include "lc_splinepoints.h"
#include "rs_arc.h"
#include "rs_circle.h"
#include "rs_dimaligned.h"
#include "rs_dimangular.h"
#include "rs_dimdiametric.h"
#include "rs_dimension.h"
#include "rs_dimlinear.h"
#include "rs_dimradial.h"
#include "rs_ellipse.h"
#include "rs_entity.h"
#include "rs_hatch.h"
#include "rs_insert.h"
#include "rs_layer.h"
#include "rs_line.h"
#include "rs_math.h"
#include "rs_mtext.h"
#include "rs_point.h"
#include "rs_polyline.h"
#include "rs_spline.h"
#include "rs_text.h"

namespace LC_RemoteApiEntityJson {

namespace {

// RS2::LineWidth values other than ByLayer/ByBlock/Default, in hundredths of a mm.
constexpr int StandardWidths[] = {0,  5,  9,  13, 15, 18,  20,  25,  30,  35,  40,  50,
                                  53, 60, 70, 80, 90, 100, 106, 120, 140, 158, 200, 211};

// The largest integer a JSON number (an IEEE double) holds exactly.
constexpr double MaxExactInteger = 9007199254740992.0;

double degrees(const double radians) {
    return RS_Math::rad2deg(radians);
}

// Angles on the wire are in [0, 360).
double degreesNormalized(const double radians) {
    return RS_Math::rad2deg(RS_Math::correctAngle(radians));
}

QString hAlignName(const RS_TextData::HAlign align) {
    switch (align) {
        case RS_TextData::HACenter:
            return QStringLiteral("center");
        case RS_TextData::HARight:
            return QStringLiteral("right");
        case RS_TextData::HAAligned:
            return QStringLiteral("aligned");
        case RS_TextData::HAMiddle:
            return QStringLiteral("middle");
        case RS_TextData::HAFit:
            return QStringLiteral("fit");
        case RS_TextData::HALeft:
        default:
            return QStringLiteral("left");
    }
}

QString vAlignName(const RS_TextData::VAlign align) {
    switch (align) {
        case RS_TextData::VABottom:
            return QStringLiteral("bottom");
        case RS_TextData::VAMiddle:
            return QStringLiteral("middle");
        case RS_TextData::VATop:
            return QStringLiteral("top");
        case RS_TextData::VABaseline:
        default:
            return QStringLiteral("baseline");
    }
}

QString attachmentName(const RS_MTextData::VAlign valign, const RS_MTextData::HAlign halign) {
    QString v;
    switch (valign) {
        case RS_MTextData::VAMiddle:
            v = QStringLiteral("middle");
            break;
        case RS_MTextData::VABottom:
            v = QStringLiteral("bottom");
            break;
        case RS_MTextData::VATop:
        default:
            v = QStringLiteral("top");
            break;
    }
    QString h;
    switch (halign) {
        case RS_MTextData::HACenter:
            h = QStringLiteral("center");
            break;
        case RS_MTextData::HARight:
            h = QStringLiteral("right");
            break;
        case RS_MTextData::HALeft:
        default:
            h = QStringLiteral("left");
            break;
    }
    return v + QLatin1Char('_') + h;
}

QString cutText(const QString& text, const Detail detail) {
    if (detail == Detail::Summary && text.size() > SummaryTextLength) {
        return text.left(SummaryTextLength);
    }
    return text;
}

double bulgeOf(const RS_Entity* segment) {
    if (segment == nullptr) {
        return 0.0;
    }
    switch (segment->rtti()) {
        case RS2::EntityArc:
            return static_cast<const RS_Arc*>(segment)->getBulge();
        case RS2::EntityEllipse:
            return static_cast<const RS_Ellipse*>(segment)->getBulge();
        default:
            return 0.0;
    }
}

QJsonValue vertexValue(const RS_Vector& v, const double bulge) {
    if (!v.valid || !std::isfinite(v.x) || !std::isfinite(v.y)) {
        return QJsonValue::Null;
    }
    QJsonArray vertex{v.x, v.y};
    if (std::abs(bulge) > RS_TOLERANCE && std::isfinite(bulge)) {
        vertex.append(bulge);
    }
    return vertex;
}

/** Vertices of a polyline as [x, y] or [x, y, bulge] (the bulge of the segment that starts there). */
QJsonArray polylineVertices(const RS_Polyline* polyline) {
    QJsonArray vertices;
    QList<const RS_Entity*> segments;
    for (const RS_Entity* e : *polyline) {
        if (e != nullptr && e->isAtomic()) {
            segments.append(e);
        }
    }
    if (segments.isEmpty()) {
        const RS_Vector start = polyline->getStartpoint();
        if (start.valid) {
            vertices.append(vertexValue(start, 0.0));
        }
        return vertices;
    }
    for (const RS_Entity* segment : std::as_const(segments)) {
        vertices.append(vertexValue(segment->getStartpoint(), bulgeOf(segment)));
    }
    // An open polyline ends at the last segment's end; so does a closed one whose
    // closing segment LibreCAD left out (shorter than 1e-4): that vertex is real.
    if (!polyline->isClosed() || missingClosingChord(polyline).first.valid) {
        vertices.append(vertexValue(segments.last()->getEndpoint(), 0.0));
    }
    return vertices;
}

int polylineVertexCount(const RS_Polyline* polyline) {
    int segments = 0;
    for (const RS_Entity* e : *polyline) {
        if (e != nullptr && e->isAtomic()) {
            ++segments;
        }
    }
    if (segments == 0) {
        return polyline->getStartpoint().valid ? 1 : 0;
    }
    return polyline->isClosed() && !missingClosingChord(polyline).first.valid ? segments : segments + 1;
}

QJsonArray vectorPoints(const std::vector<RS_Vector>& points) {
    QJsonArray out;
    for (const RS_Vector& p : points) {
        out.append(pointValue(p));
    }
    return out;
}

void addDimensionGeometry(RS_Entity* e, QJsonObject& o) {
    auto* dim = static_cast<RS_Dimension*>(e);
    const RS2::EntityType rtti = e->rtti();
    o.insert(QStringLiteral("dim_type"), dimensionTypeName(rtti));
    switch (rtti) {
        case RS2::EntityDimLinear: {
            const auto* d = static_cast<RS_DimLinear*>(e);
            o.insert(QStringLiteral("p1"), pointValue(d->getExtensionPoint1()));
            o.insert(QStringLiteral("p2"), pointValue(d->getExtensionPoint2()));
            o.insert(QStringLiteral("line_point"), pointValue(d->getDefinitionPoint()));
            o.insert(QStringLiteral("angle_deg"), degreesNormalized(d->getAngle()));
            break;
        }
        case RS2::EntityDimAligned: {
            auto* d = static_cast<RS_DimAligned*>(e);
            o.insert(QStringLiteral("p1"), pointValue(d->getEData().extensionPoint1));
            o.insert(QStringLiteral("p2"), pointValue(d->getEData().extensionPoint2));
            o.insert(QStringLiteral("line_point"), pointValue(d->getDefinitionPoint()));
            break;
        }
        case RS2::EntityDimRadial: {
            const auto* d = static_cast<RS_DimRadial*>(e);
            o.insert(QStringLiteral("center"), pointValue(d->getDefinitionPoint()));
            o.insert(QStringLiteral("point"), pointValue(d->getRadialDefinitionPoint()));
            o.insert(QStringLiteral("leader_length"), d->getLeader());
            break;
        }
        case RS2::EntityDimDiametric: {
            const auto* d = static_cast<RS_DimDiametric*>(e);
            o.insert(QStringLiteral("point"), pointValue(d->getDiametricDefinitionPoint()));
            o.insert(QStringLiteral("opposite_point"), pointValue(d->getDefinitionPoint()));
            break;
        }
        case RS2::EntityDimAngular: {
            const auto* d = static_cast<RS_DimAngular*>(e);
            // DXF: line 1 is P2-P1, line 2 is P-P3 (RS_DimAngular::calcDimension)
            o.insert(QStringLiteral("line1"), QJsonArray{pointValue(d->getDefinitionPoint2()), pointValue(d->getDefinitionPoint1())});
            o.insert(QStringLiteral("line2"), QJsonArray{pointValue(d->getDefinitionPoint()), pointValue(d->getDefinitionPoint3())});
            o.insert(QStringLiteral("arc_point"), pointValue(d->getDefinitionPoint4()));
            break;
        }
        default:
            o.insert(QStringLiteral("definition_point"), pointValue(dim->getDefinitionPoint()));
            break;
    }
    o.insert(QStringLiteral("text"), dim->getText());
    o.insert(QStringLiteral("style"), dim->getStyle());
}

QString readOnlyClassName(const RS2::EntityType rtti) {
    switch (rtti) {
        case RS2::EntityImage:
            return QStringLiteral("RS_Image");
        case RS2::EntityDimLeader:
            return QStringLiteral("RS_Leader");
        case RS2::EntityMLeader:
            return QStringLiteral("LC_MLeader");
        case RS2::EntitySolid:
            return QStringLiteral("RS_Solid");
        case RS2::EntityParabola:
            return QStringLiteral("LC_Parabola");
        case RS2::EntityHyperbola:
            return QStringLiteral("LC_Hyperbola");
        case RS2::EntityConstructionLine:
            return QStringLiteral("RS_ConstructionLine");
        case RS2::EntityWipeout:
            return QStringLiteral("LC_Wipeout");
        case RS2::EntityTolerance:
            return QStringLiteral("LC_Tolerance");
        case RS2::EntityDimArc:
            return QStringLiteral("LC_DimArc");
        case RS2::EntityDimOrdinate:
            return QStringLiteral("LC_DimOrdinate");
        default:
            return QStringLiteral("rtti_%1").arg(static_cast<unsigned>(rtti));
    }
}

/** The input geometry of e, plus the derived read-only fields when derived is set. */
void addGeometry(RS_Entity* e, QJsonObject& o, const Detail detail, const bool derived) {
    const auto put = [&o](const char* key, const QJsonValue& value) {
        o.insert(QLatin1String(key), value);
    };
    switch (e->rtti()) {
        case RS2::EntityLine: {
            const auto* l = static_cast<RS_Line*>(e);
            put("start", pointValue(l->getStartpoint()));
            put("end", pointValue(l->getEndpoint()));
            if (derived) {
                put("length", l->getLength());
                put("angle_deg", degreesNormalized(l->getAngle1()));
            }
            break;
        }
        case RS2::EntityCircle: {
            const auto* c = static_cast<RS_Circle*>(e);
            put("center", pointValue(c->getCenter()));
            put("radius", c->getRadius());
            if (derived) {
                put("length", 2.0 * M_PI * c->getRadius());
                put("area", M_PI * c->getRadius() * c->getRadius());
            }
            break;
        }
        case RS2::EntityArc: {
            const auto* a = static_cast<RS_Arc*>(e);
            put("center", pointValue(a->getCenter()));
            put("radius", a->getRadius());
            put("start_angle_deg", degreesNormalized(a->getAngle1()));
            put("end_angle_deg", degreesNormalized(a->getAngle2()));
            put("reversed", a->isReversed());
            if (derived) {
                put("start_point", pointValue(a->getStartpoint()));
                put("end_point", pointValue(a->getEndpoint()));
                put("length", a->getLength());
            }
            break;
        }
        case RS2::EntityEllipse: {
            const auto* el = static_cast<RS_Ellipse*>(e);
            put("center", pointValue(el->getCenter()));
            put("major_axis", pointValue(el->getMajorP()));
            put("ratio", el->getRatio());
            if (!isFullEllipse(el)) {
                put("start_angle_deg", degreesNormalized(el->getAngle1()));
                put("end_angle_deg", degreesNormalized(el->getAngle2()));
            }
            else {
                put("start_angle_deg", 0.0);
                put("end_angle_deg", 360.0);
            }
            put("reversed", el->isReversed());
            if (derived) {
                put("length", el->getLength());
            }
            break;
        }
        case RS2::EntityPoint: {
            put("position", pointValue(static_cast<RS_Point*>(e)->getPos()));
            break;
        }
        case RS2::EntityPolyline: {
            const auto* pl = static_cast<RS_Polyline*>(e);
            if (detail == Detail::Full) {
                put("vertices", polylineVertices(pl));
            }
            put("closed", pl->isClosed());
            if (derived) {
                put("vertex_count", polylineVertexCount(pl));
                put("length", polylineLength(pl));
                if (pl->isClosed()) {
                    put("area", closedPolylineArea(pl));
                }
                if (hasEllipticSegments(pl)) {
                    // the bulges above describe circular arcs: these segments are not
                    put("elliptic_segments", true);
                }
            }
            break;
        }
        case RS2::EntitySpline: {
            const auto* sp = static_cast<RS_Spline*>(e);
            const std::vector<RS_Vector> controlPoints = sp->getControlPoints();
            if (detail == Detail::Full) {
                put("control_points", vectorPoints(controlPoints));
            }
            put("degree", sp->getDegree());
            put("closed", sp->isClosed());
            if (derived) {
                put("vertex_count", static_cast<int>(controlPoints.size()));
            }
            break;
        }
        case RS2::EntitySplinePoints: {
            const auto* sp = static_cast<LC_SplinePoints*>(e);
            const std::vector<RS_Vector>& points = sp->getPoints();
            if (detail == Detail::Full) {
                put("fit_points", vectorPoints(points));
            }
            put("closed", sp->isClosed());
            if (derived) {
                put("vertex_count", static_cast<int>(points.size()));
            }
            break;
        }
        case RS2::EntityText: {
            const auto* t = static_cast<RS_Text*>(e);
            put("position", pointValue(t->getInsertionPoint()));
            put("text", cutText(t->getText(), detail));
            put("height", t->getHeight());
            put("angle_deg", degreesNormalized(t->getAngle()));
            put("halign", hAlignName(t->getHAlign()));
            put("valign", vAlignName(t->getVAlign()));
            put("style", t->getStyle());
            put("width_factor", t->getWidthRel());
            if (t->getHAlign() == RS_TextData::HAAligned || t->getHAlign() == RS_TextData::HAFit) {
                put("second_point", pointValue(t->getSecondPoint()));
            }
            break;
        }
        case RS2::EntityMText: {
            const auto* t = static_cast<RS_MText*>(e);
            put("position", pointValue(t->getInsertionPoint()));
            put("text", cutText(t->getText(), detail));
            put("height", t->getHeight());
            put("width", t->getWidth());
            put("angle_deg", degreesNormalized(t->getAngle()));
            put("attachment", attachmentName(t->getVAlign(), t->getHAlign()));
            put("line_spacing", t->getLineSpacingFactor());
            put("style", t->getStyle());
            break;
        }
        case RS2::EntityDimLinear:
        case RS2::EntityDimAligned:
        case RS2::EntityDimRadial:
        case RS2::EntityDimDiametric:
        case RS2::EntityDimAngular:
        case RS2::EntityDimArc:
        case RS2::EntityDimOrdinate: {
            addDimensionGeometry(e, o);
            if (derived) {
                const auto* dim = static_cast<RS_Dimension*>(e);
                const double m = dim->getMeasurement();
                put("measurement", e->rtti() == RS2::EntityDimAngular ? degrees(m) : m);
            }
            break;
        }
        case RS2::EntityHatch: {
            const auto* h = static_cast<RS_Hatch*>(e);
            put("pattern", h->isSolid() ? QStringLiteral("SOLID") : h->getPattern());
            put("scale", h->getScale());
            put("angle_deg", degreesNormalized(h->getAngle()));
            if (derived) {
                put("area", h->getTotalArea());
                // every loop given, holes included (countLoops() counts outer loops only)
                put("loop_count", h->countAllLoops());
            }
            break;
        }
        case RS2::EntityInsert: {
            const auto* in = static_cast<RS_Insert*>(e);
            put("block", in->getName());
            put("position", pointValue(in->getInsertionPoint()));
            const RS_Vector scale = in->getScale();
            put("scale", QJsonArray{scale.x, scale.y});
            put("angle_deg", degreesNormalized(in->getAngle()));
            put("columns", in->getCols());
            put("rows", in->getRows());
            const RS_Vector spacing = in->getSpacing();
            put("spacing", QJsonArray{spacing.x, spacing.y});
            break;
        }
        default:
            if (derived) {
                put("rs_type", readOnlyClassName(e->rtti()));
            }
            break;
    }
}

} // namespace

// --- paths and failures ------------------------------------------------------

QString joinPath(const QString& base, const QString& key) {
    if (base.isEmpty()) {
        return key;
    }
    return base + QLatin1Char('.') + key;
}

QString indexPath(const QString& base, const int index) {
    return base + QLatin1Char('[') + QString::number(index) + QLatin1Char(']');
}

void throwBadParam(const QString& param, const QString& detail, const QJsonArray& allowed) {
    QJsonObject extra{{QStringLiteral("param"), param}};
    if (!allowed.isEmpty()) {
        extra.insert(QStringLiteral("allowed"), allowed);
    }
    throw LC_ApiError(400, QStringLiteral("bad_param"), detail, extra);
}

void throwMissingParam(const QString& param) {
    throw LC_ApiError(400, QStringLiteral("missing_param"), QStringLiteral("'%1' is required").arg(param),
                      {{QStringLiteral("param"), param}});
}

void throwUnknownField(const QString& field, const QString& detail) {
    throw LC_ApiError(400, QStringLiteral("unknown_field"),
                      detail.isEmpty() ? QStringLiteral("unknown field '%1'").arg(field) : detail,
                      {{QStringLiteral("field"), field}});
}

void throwInvalidEntity(const QString& param, const QString& detail) {
    throw LC_ApiError(422, QStringLiteral("invalid_entity"), detail,
                      {{QStringLiteral("errors"), QJsonObject{{param, detail}}}});
}

// --- values ------------------------------------------------------------------

QJsonValue pointValue(const RS_Vector& v) {
    return lcJsonPoint(v);
}

QJsonArray pointsValue(const QList<RS_Vector>& points) {
    QJsonArray out;
    for (const RS_Vector& p : points) {
        out.append(pointValue(p));
    }
    return out;
}

QJsonObject boxValue(const RS_Vector& min, const RS_Vector& max) {
    return {{QStringLiteral("min"), pointValue(min)}, {QStringLiteral("max"), pointValue(max)}};
}

QString colorName(const RS_Color& color) {
    if (color.getFlag(RS2::FlagByLayer)) {
        return QStringLiteral("ByLayer");
    }
    if (color.getFlag(RS2::FlagByBlock)) {
        return QStringLiteral("ByBlock");
    }
    return QColor(color.red(), color.green(), color.blue()).name(QColor::HexRgb);
}

int aciNumber(const RS_Color& color) {
    if (color.getFlag(RS2::FlagByLayer) || color.getFlag(RS2::FlagByBlock)) {
        return -1;
    }
    int rgb = -1;
    const int number = LC_ColorNumbers::colorToNumber(color, &rgb);
    return (rgb == -1 && number >= 1 && number <= 255) ? number : -1;
}

QJsonValue lineWidthValue(const RS2::LineWidth width) {
    switch (width) {
        case RS2::WidthByLayer:
            return QStringLiteral("ByLayer");
        case RS2::WidthByBlock:
            return QStringLiteral("ByBlock");
        case RS2::WidthDefault:
            return QStringLiteral("Default");
        default:
            if (static_cast<int>(width) < 0) {
                return QStringLiteral("Default");
            }
            return static_cast<int>(width) / 100.0;
    }
}

QString lineTypeName(const RS_Pen& pen) {
    if (pen.isLineTypeByLayer()) {
        return QStringLiteral("ByLayer");
    }
    if (pen.isLineTypeByBlock()) {
        return QStringLiteral("ByBlock");
    }
    return pen.getLineTypeName();
}

QJsonArray standardLineWidthsMm() {
    QJsonArray widths;
    for (const int w : StandardWidths) {
        widths.append(w / 100.0);
    }
    return widths;
}

QStringList lineTypeNames(const bool withByLayerAndByBlock) {
    QStringList names;
    if (withByLayerAndByBlock) {
        names << QStringLiteral("ByLayer") << QStringLiteral("ByBlock");
    }
    for (int t = RS2::SolidLine; t <= RS2::PhantomLineX2; ++t) {
        if (t == RS2::LineTypeUnchanged || t == RS2::LineSelected) {
            continue;
        }
        names << LC_LineTypeNames::lineTypeToName(static_cast<RS2::LineType>(t));
    }
    return names;
}

double parseNumber(const QJsonValue& v, const QString& path) {
    if (!v.isDouble() || !std::isfinite(v.toDouble())) {
        throwBadParam(path, QStringLiteral("'%1' must be a number").arg(path));
    }
    return v.toDouble();
}

RS_Vector parsePoint(const QJsonValue& v, const QString& path) {
    const QJsonArray a = v.toArray();
    if (!v.isArray() || a.size() < 2 || a.size() > 3) {
        throwBadParam(path, QStringLiteral("'%1' must be a point [x, y]").arg(path));
    }
    for (int i = 0; i < a.size(); ++i) {
        if (!a.at(i).isDouble() || !std::isfinite(a.at(i).toDouble())) {
            throwBadParam(path, QStringLiteral("'%1' must be a point [x, y] of finite numbers").arg(path));
        }
    }
    return {a.at(0).toDouble(), a.at(1).toDouble()};
}

QList<RS_Vector> parsePoints(const QJsonValue& v, const QString& path, const int minCount) {
    if (!v.isArray()) {
        throwBadParam(path, QStringLiteral("'%1' must be an array of points").arg(path));
    }
    const QJsonArray a = v.toArray();
    QList<RS_Vector> points;
    for (int i = 0; i < a.size(); ++i) {
        points.append(parsePoint(a.at(i), indexPath(path, i)));
    }
    if (points.size() < minCount) {
        throwInvalidEntity(path, QStringLiteral("needs at least %1 points").arg(minCount));
    }
    return points;
}

RS_Color parseColor(const QJsonValue& v, const QString& path, const bool allowByLayerAndByBlock) {
    QJsonArray allowed;
    if (allowByLayerAndByBlock) {
        allowed << QStringLiteral("ByLayer") << QStringLiteral("ByBlock");
    }
    allowed << QStringLiteral("#rrggbb") << QStringLiteral("1..255 (ACI)");
    if (v.isString()) {
        const QString text = v.toString().trimmed();
        const QString lower = text.toLower();
        if (allowByLayerAndByBlock && lower == QLatin1String("bylayer")) {
            return RS_Color(RS2::FlagByLayer);
        }
        if (allowByLayerAndByBlock && lower == QLatin1String("byblock")) {
            return RS_Color(RS2::FlagByBlock);
        }
        static const QRegularExpression hexColor(QStringLiteral("^#[0-9a-fA-F]{6}$"));
        if (hexColor.match(text).hasMatch()) {
            return RS_Color(QColor(text));
        }
        throwBadParam(path, QStringLiteral("'%1' is not a color: use %2").arg(text, allowByLayerAndByBlock
                                                                                         ? QStringLiteral("ByLayer, ByBlock, #rrggbb or an ACI number 1..255")
                                                                                         : QStringLiteral("#rrggbb or an ACI number 1..255")),
                      allowed);
    }
    if (v.isDouble()) {
        const double number = v.toDouble();
        if (std::isfinite(number) && number == std::floor(number) && number >= 1 && number <= 255) {
            return LC_ColorNumbers::numberToColor(static_cast<int>(number));
        }
        throwBadParam(path, QStringLiteral("ACI color numbers are 1..255"), allowed);
    }
    throwBadParam(path, QStringLiteral("'%1' must be a color string or an ACI number").arg(path), allowed);
}

RS2::LineWidth parseLineWidth(const QJsonValue& v, const QString& path, const bool allowByLayerAndByBlock) {
    QJsonArray allowed;
    if (allowByLayerAndByBlock) {
        allowed << QStringLiteral("ByLayer") << QStringLiteral("ByBlock");
    }
    allowed << QStringLiteral("Default");
    for (const auto& w : standardLineWidthsMm()) {
        allowed.append(w);
    }
    if (v.isString()) {
        const QString lower = v.toString().trimmed().toLower();
        if (lower == QLatin1String("default")) {
            return RS2::WidthDefault;
        }
        if (allowByLayerAndByBlock && lower == QLatin1String("bylayer")) {
            return RS2::WidthByLayer;
        }
        if (allowByLayerAndByBlock && lower == QLatin1String("byblock")) {
            return RS2::WidthByBlock;
        }
    }
    else if (v.isDouble() && std::isfinite(v.toDouble())) {
        const double hundredths = v.toDouble() * 100.0;
        for (const int w : StandardWidths) {
            if (std::abs(hundredths - w) < 0.5) {
                return static_cast<RS2::LineWidth>(w);
            }
        }
    }
    QStringList widths;
    for (const int w : StandardWidths) {
        widths << QString::number(w / 100.0);
    }
    throwBadParam(path, QStringLiteral("not a standard lineweight; use %1Default or one of these widths in mm: %2")
                            .arg(allowByLayerAndByBlock ? QStringLiteral("ByLayer, ByBlock, ") : QString(), widths.join(QStringLiteral(", "))),
                  allowed);
}

RS2::LineType parseLineType(const QJsonValue& v, const QString& path, const bool allowByLayerAndByBlock) {
    const QStringList names = lineTypeNames(allowByLayerAndByBlock);
    const QString text = v.isString() ? v.toString().trimmed() : QString();
    const QString upper = text.toUpper();
    if (!text.isEmpty()) {
        if (upper == QLatin1String("BYLAYER") || upper == QLatin1String("BYBLOCK")) {
            if (allowByLayerAndByBlock) {
                return upper == QLatin1String("BYLAYER") ? RS2::LineByLayer : RS2::LineByBlock;
            }
        }
        else {
            const RS2::LineType type = LC_LineTypeNames::nameToLineType(text);
            // nameToLineType answers SolidLine for any name it does not know
            const bool known = type != RS2::SolidLine || upper == QLatin1String("CONTINUOUS") || upper == QLatin1String("ACAD_ISO01W100");
            if (known && type != RS2::LineByLayer && type != RS2::LineByBlock) {
                return type;
            }
        }
    }
    throwBadParam(path, QStringLiteral("unknown line type '%1'").arg(text), QJsonArray::fromStringList(names));
}

unsigned long long parseId(const QJsonValue& v, const QString& path) {
    if (v.isDouble()) {
        const double d = v.toDouble();
        if (std::isfinite(d) && d == std::floor(d) && d >= 1 && d <= MaxExactInteger) {
            return static_cast<unsigned long long>(d);
        }
    }
    throwBadParam(path, QStringLiteral("'%1' must be an entity id (a positive integer)").arg(path));
}

QList<unsigned long long> parseIds(const QJsonValue& v, const QString& path, const int maxCount) {
    if (!v.isArray()) {
        throwBadParam(path, QStringLiteral("'%1' must be an array of entity ids").arg(path));
    }
    const QJsonArray a = v.toArray();
    if (a.isEmpty()) {
        throwBadParam(path, QStringLiteral("'%1' must list at least one entity id").arg(path));
    }
    if (maxCount > 0 && a.size() > maxCount) {
        throw LC_ApiError(422, QStringLiteral("too_many"), QStringLiteral("at most %1 ids per request").arg(maxCount),
                          {{QStringLiteral("limit"), maxCount}, {QStringLiteral("param"), path}});
    }
    QList<unsigned long long> ids;
    for (int i = 0; i < a.size(); ++i) {
        const unsigned long long id = parseId(a.at(i), indexPath(path, i));
        if (!ids.contains(id)) {
            ids.append(id);
        }
    }
    return ids;
}

unsigned long long parseIdText(const QString& text, const QString& param) {
    bool ok = false;
    const unsigned long long id = text.toULongLong(&ok);
    if (!ok || id == 0 || static_cast<double>(id) > MaxExactInteger) {
        throwBadParam(param, QStringLiteral("'%1' is not an entity id").arg(text));
    }
    return id;
}

// --- entities ------------------------------------------------------------------

QJsonValue idValue(const RS_Entity* e) {
    return static_cast<qint64>(e->lineage());
}

QString typeName(const RS_Entity* e) {
    return lcEntityTypeName(e->rtti());
}

QStringList allTypeNames() {
    return creatableTypeNames() + QStringList{QStringLiteral("image"), QStringLiteral("leader"), QStringLiteral("mleader"),
                                              QStringLiteral("solid"), QStringLiteral("parabola"), QStringLiteral("hyperbola"),
                                              QStringLiteral("other")};
}

QStringList creatableTypeNames() {
    return {QStringLiteral("line"),     QStringLiteral("circle"), QStringLiteral("arc"),   QStringLiteral("ellipse"),
            QStringLiteral("point"),    QStringLiteral("polyline"), QStringLiteral("spline"), QStringLiteral("text"),
            QStringLiteral("mtext"),    QStringLiteral("dimension"), QStringLiteral("hatch"), QStringLiteral("insert")};
}

QString dimensionTypeName(const RS2::EntityType rtti) {
    switch (rtti) {
        case RS2::EntityDimLinear:
            return QStringLiteral("linear");
        case RS2::EntityDimAligned:
            return QStringLiteral("aligned");
        case RS2::EntityDimRadial:
            return QStringLiteral("radial");
        case RS2::EntityDimDiametric:
            return QStringLiteral("diametric");
        case RS2::EntityDimAngular:
            return QStringLiteral("angular");
        case RS2::EntityDimArc:
            return QStringLiteral("arc");
        case RS2::EntityDimOrdinate:
            return QStringLiteral("ordinate");
        default:
            return {};
    }
}

bool isFullEllipse(const RS_Ellipse* ellipse) {
    if (!ellipse->isEllipticArc()) {
        return true;
    }
    return std::abs(std::remainder(ellipse->getAngle2() - ellipse->getAngle1(), 2.0 * M_PI)) < RS_TOLERANCE_ANGLE;
}

std::pair<RS_Vector, RS_Vector> missingClosingChord(const RS_Polyline* polyline) {
    if (!polyline->isClosed()) {
        return {RS_Vector(false), RS_Vector(false)};
    }
    const RS_Entity* first = nullptr;
    const RS_Entity* last = nullptr;
    for (const RS_Entity* e : *polyline) {
        if (e != nullptr && e->isAtomic()) {
            if (first == nullptr) {
                first = e;
            }
            last = e;
        }
    }
    if (first == nullptr) {
        return {RS_Vector(false), RS_Vector(false)};
    }
    const RS_Vector end = last->getEndpoint();
    const RS_Vector start = first->getStartpoint();
    if (!end.valid || !start.valid || end.distanceTo(start) <= RS_TOLERANCE) {
        return {RS_Vector(false), RS_Vector(false)};
    }
    return {end, start};
}

double closedPolylineArea(const RS_Polyline* polyline) {
    // the line integral of x dy along the segments in their order, plus the
    // chord LibreCAD leaves out: without it the result depends on where the
    // polyline is (an open chain is not translation invariant)
    double integral = 0.0;
    for (const RS_Entity* e : *polyline) {
        if (e != nullptr && e->isAtomic()) {
            integral += e->areaLineIntegral();
        }
    }
    const auto [from, to] = missingClosingChord(polyline);
    if (from.valid) {
        integral += 0.5 * (to.y - from.y) * (from.x + to.x);
    }
    return std::abs(integral);
}

double polylineLength(const RS_Polyline* polyline) {
    double length = polyline->getLength();
    const auto [from, to] = missingClosingChord(polyline);
    if (from.valid) {
        length += from.distanceTo(to);
    }
    return length;
}

bool hasEllipticSegments(const RS_Polyline* polyline) {
    for (const RS_Entity* e : *polyline) {
        if (e != nullptr && e->rtti() == RS2::EntityEllipse) {
            return true;
        }
    }
    return false;
}

QJsonObject entityToJson(RS_Entity* e, const Detail detail) {
    QJsonObject o;
    o.insert(QStringLiteral("id"), idValue(e));
    o.insert(QStringLiteral("type"), typeName(e));
    const quint32 handle = e->sourceHandle();
    o.insert(QStringLiteral("handle"), handle != 0 ? QJsonValue(QString::number(handle, 16).toUpper()) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("visible"), e->isVisible());
    o.insert(QStringLiteral("locked"), e->isLocked());
    o.insert(QStringLiteral("selected"), e->isSelected());
    o.insert(QStringLiteral("bbox"), boxValue(e->getMin(), e->getMax()));

    const RS_Layer* layer = e->getLayer(true);
    o.insert(QStringLiteral("layer"), layer != nullptr ? QJsonValue(layer->getName()) : QJsonValue(QJsonValue::Null));
    const RS_Pen pen = e->getPen(false);
    o.insert(QStringLiteral("color"), colorName(pen.getColor()));
    o.insert(QStringLiteral("linetype"), lineTypeName(pen));
    o.insert(QStringLiteral("lineweight"), lineWidthValue(pen.getWidth()));

    addGeometry(e, o, detail, true);
    return o;
}

QJsonObject geometryFields(RS_Entity* e) {
    QJsonObject o;
    addGeometry(e, o, Detail::Full, false);
    return o;
}

Attributes Attributes::read(const QJsonObject& object, const QString& path) {
    Attributes attrs;
    const auto layerValue = object.value(QStringLiteral("layer"));
    if (!layerValue.isUndefined()) {
        if (!layerValue.isString() || layerValue.toString().isEmpty()) {
            throwBadParam(joinPath(path, QStringLiteral("layer")), QStringLiteral("'layer' must be a layer name"));
        }
        attrs.layer = layerValue.toString();
    }
    const auto colorValue = object.value(QStringLiteral("color"));
    if (!colorValue.isUndefined()) {
        attrs.color = parseColor(colorValue, joinPath(path, QStringLiteral("color")), true);
    }
    const auto lineTypeValue = object.value(QStringLiteral("linetype"));
    if (!lineTypeValue.isUndefined()) {
        attrs.lineType = parseLineType(lineTypeValue, joinPath(path, QStringLiteral("linetype")), true);
    }
    const auto lineWidthJson = object.value(QStringLiteral("lineweight"));
    if (!lineWidthJson.isUndefined()) {
        attrs.lineWidth = parseLineWidth(lineWidthJson, joinPath(path, QStringLiteral("lineweight")), true);
    }
    return attrs;
}

RS_Pen Attributes::applyTo(RS_Pen pen) const {
    if (color.has_value()) {
        pen.setColor(*color);
    }
    if (lineType.has_value()) {
        pen.setLineType(*lineType);
    }
    if (lineWidth.has_value()) {
        pen.setWidth(*lineWidth);
    }
    return pen;
}

} // namespace LC_RemoteApiEntityJson
