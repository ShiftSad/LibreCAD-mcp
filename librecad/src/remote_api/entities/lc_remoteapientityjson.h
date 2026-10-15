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

#ifndef LC_REMOTEAPIENTITYJSON_H
#define LC_REMOTEAPIENTITYJSON_H

// JSON of entities and their attributes for the Remote API (design sections
// 2.3 and 2.6): entity output in summary and full detail, and the strict
// converters of the attribute values (color, linetype, lineweight, points,
// ids). Every parse failure throws LC_ApiError with the path of the value.

#include <optional>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include "rs.h"
#include "rs_color.h"
#include "rs_pen.h"
#include "rs_vector.h"

class RS_Ellipse;
class RS_Entity;
class RS_Layer;
class RS_Polyline;

namespace LC_RemoteApiEntityJson {

enum class Detail {
    Summary,   // polylines and splines give vertex_count only, text is cut to 200 characters
    Full
};

/** Longest text of an entity in summary detail. */
constexpr int SummaryTextLength = 200;

// --- paths and failures ------------------------------------------------------

/** "entities[2]" + "radius" gives "entities[2].radius"; an empty base gives the key. */
QString joinPath(const QString& base, const QString& key);
/** "entities" + 2 gives "entities[2]". */
QString indexPath(const QString& base, int index);

/** 400 bad_param with "param" (and "allowed" when given). */
[[noreturn]] void throwBadParam(const QString& param, const QString& detail, const QJsonArray& allowed = {});
/** 400 missing_param with "param". */
[[noreturn]] void throwMissingParam(const QString& param);
/** 400 unknown_field with "field". */
[[noreturn]] void throwUnknownField(const QString& field, const QString& detail = {});
/** 422 invalid_entity with "errors": {param: detail}. */
[[noreturn]] void throwInvalidEntity(const QString& param, const QString& detail);

// --- values ------------------------------------------------------------------

/** [x, y], or null for an invalid or non-finite vector. */
QJsonValue pointValue(const RS_Vector& v);
QJsonArray pointsValue(const QList<RS_Vector>& points);
/** {"min": [x, y], "max": [x, y]} */
QJsonObject boxValue(const RS_Vector& min, const RS_Vector& max);
/** "ByLayer", "ByBlock" or "#rrggbb". */
QString colorName(const RS_Color& color);
/** The ACI number of a color that has one exactly, or -1. */
int aciNumber(const RS_Color& color);
/** "ByLayer", "ByBlock", "Default" or the width in mm. */
QJsonValue lineWidthValue(RS2::LineWidth width);
/** "ByLayer", "ByBlock" or the line type name. */
QString lineTypeName(const RS_Pen& pen);
/** The standard widths in mm, ascending. */
QJsonArray standardLineWidthsMm();
/** The line type names accepted on input (ByLayer and ByBlock only when allowed). */
QStringList lineTypeNames(bool withByLayerAndByBlock);

double parseNumber(const QJsonValue& v, const QString& path);
RS_Vector parsePoint(const QJsonValue& v, const QString& path);
QList<RS_Vector> parsePoints(const QJsonValue& v, const QString& path, int minCount);
/** Color: "ByLayer" | "ByBlock" | "#rrggbb" | ACI 1..255. Layers allow neither ByLayer nor ByBlock. */
RS_Color parseColor(const QJsonValue& v, const QString& path, bool allowByLayerAndByBlock);
/** Lineweight: "ByLayer" | "ByBlock" | "Default" | mm snapped to a standard width. */
RS2::LineWidth parseLineWidth(const QJsonValue& v, const QString& path, bool allowByLayerAndByBlock);
/** Line type: "ByLayer" | "ByBlock" | a name of LC_LineTypeNames ("Continuous", "Dashed", ...). */
RS2::LineType parseLineType(const QJsonValue& v, const QString& path, bool allowByLayerAndByBlock);
/** An entity id: a positive integer. */
unsigned long long parseId(const QJsonValue& v, const QString& path);
/** A non-empty list of entity ids, at most maxCount (0: no limit); duplicates are dropped. */
QList<unsigned long long> parseIds(const QJsonValue& v, const QString& path, int maxCount = 0);
/** An id from a path parameter ("/entities/{id}"). */
unsigned long long parseIdText(const QString& text, const QString& param);

// --- entities ------------------------------------------------------------------

/** The entity's id on the wire: its lineage. */
QJsonValue idValue(const RS_Entity* e);
/** The wire type name (section 2.3). */
QString typeName(const RS_Entity* e);
/** The wire names of every type, and of the types that can be created. */
QStringList allTypeNames();
QStringList creatableTypeNames();
/** The dim_type of a dimension entity ("linear", "aligned", "radial", "diametric", "angular", "arc", "ordinate"). */
QString dimensionTypeName(RS2::EntityType rtti);

/**
 * Whether an ellipse is a full one: not an arc, or an arc whose start and end
 * are the same angle (files store full ellipses that way too; LibreCAD draws
 * them full).
 */
bool isFullEllipse(const RS_Ellipse* ellipse);

/**
 * The closing chord of a closed polyline that LibreCAD leaves out (RS_Polyline
 * skips a closing segment shorter than 1e-4): from the end of the last segment
 * to the start. Invalid vectors when there is none.
 */
std::pair<RS_Vector, RS_Vector> missingClosingChord(const RS_Polyline* polyline);
/** Area enclosed by a closed polyline, the missing closing chord included. */
double closedPolylineArea(const RS_Polyline* polyline);
/** Length of a polyline; for a closed one, the missing closing chord included. */
double polylineLength(const RS_Polyline* polyline);
/**
 * Whether a polyline has elliptic segments (LibreCAD turns its arcs into them
 * when it scales it non-uniformly): bulges describe circular arcs only.
 */
bool hasEllipticSegments(const RS_Polyline* polyline);

/** The entity as the API shows it: attributes, geometry and derived fields. */
QJsonObject entityToJson(RS_Entity* e, Detail detail);
/**
 * Only the input geometry fields of the entity's type, in full detail: the
 * fields a spec of section 2.6 takes (without type and attributes). Used to
 * merge a PATCH body over the entity.
 */
QJsonObject geometryFields(RS_Entity* e);

/**
 * The attributes of an entity spec or PATCH body: layer, color, linetype,
 * lineweight. Fields that are not given stay empty.
 */
struct Attributes {
    std::optional<QString> layer;
    std::optional<RS_Color> color;
    std::optional<RS2::LineType> lineType;
    std::optional<RS2::LineWidth> lineWidth;

    static Attributes read(const QJsonObject& object, const QString& path);
    bool hasPen() const {
        return color.has_value() || lineType.has_value() || lineWidth.has_value();
    }
    /** pen with the given attributes replaced. */
    RS_Pen applyTo(RS_Pen pen) const;
};

} // namespace LC_RemoteApiEntityJson

#endif
