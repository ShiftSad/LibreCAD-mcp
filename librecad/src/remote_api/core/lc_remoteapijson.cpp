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

#include "lc_remoteapijson.h"

#include <cmath>

#include <QJsonArray>

#include "lc_remoteapitypes.h"

// A JSON null reads as an absent key everywhere: optional getters return their
// default, required ones report missing_param. Clients that serialise unset
// optional fields as null are therefore accepted.

namespace {

QString describeType(const QJsonValue& value) {
    switch (value.type()) {
        case QJsonValue::Bool: return QStringLiteral("a boolean");
        case QJsonValue::Double: return QStringLiteral("a number");
        case QJsonValue::String: return QStringLiteral("a string");
        case QJsonValue::Array: return QStringLiteral("an array");
        case QJsonValue::Object: return QStringLiteral("an object");
        case QJsonValue::Null: return QStringLiteral("null");
        default: return QStringLiteral("undefined");
    }
}

[[noreturn]] void badParam(const QString& param, const QString& detail, const QJsonObject& extra = {}) {
    QJsonObject fields = extra;
    fields.insert(QStringLiteral("param"), param);
    throw LC_ApiError(400, QStringLiteral("bad_param"), detail, fields);
}

// [x, y] or [x, y, z] of finite numbers; z is ignored.
std::optional<RS_Vector> toPoint(const QJsonValue& value) {
    if (!value.isArray()) {
        return std::nullopt;
    }
    const QJsonArray array = value.toArray();
    if (array.size() < 2 || array.size() > 3) {
        return std::nullopt;
    }
    for (const QJsonValue& coordinate : array) {
        if (!coordinate.isDouble() || !std::isfinite(coordinate.toDouble())) {
            return std::nullopt;
        }
    }
    return RS_Vector(array.at(0).toDouble(), array.at(1).toDouble());
}

struct TypeName {
    const char* name;
    RS2::EntityType type;
};

// The canonical class of each wire name; the first row of a name is what
// lcEntityTypeFromName returns.
constexpr TypeName TypeNames[] = {
    {"line", RS2::EntityLine},
    {"circle", RS2::EntityCircle},
    {"arc", RS2::EntityArc},
    {"ellipse", RS2::EntityEllipse},
    {"point", RS2::EntityPoint},
    {"polyline", RS2::EntityPolyline},
    {"spline", RS2::EntitySpline},
    {"spline", RS2::EntitySplinePoints},
    {"text", RS2::EntityText},
    {"mtext", RS2::EntityMText},
    {"hatch", RS2::EntityHatch},
    {"insert", RS2::EntityInsert},
    {"dimension", RS2::EntityDimLinear},
    {"dimension", RS2::EntityDimAligned},
    {"dimension", RS2::EntityDimRadial},
    {"dimension", RS2::EntityDimDiametric},
    {"dimension", RS2::EntityDimAngular},
    {"dimension", RS2::EntityDimArc},
    {"dimension", RS2::EntityDimOrdinate},
    {"image", RS2::EntityImage},
    {"leader", RS2::EntityDimLeader},
    {"mleader", RS2::EntityMLeader},
    {"solid", RS2::EntitySolid},
    {"parabola", RS2::EntityParabola},
    {"hyperbola", RS2::EntityHyperbola},
};

} // namespace

LC_JsonReader::LC_JsonReader(QJsonObject o, QString path)
    : m_object(std::move(o))
    , m_path(std::move(path)) {
}

QString LC_JsonReader::fullPath(const char* key) const {
    const QString name = QString::fromUtf8(key);
    return m_path.isEmpty() ? name : m_path + QLatin1Char('.') + name;
}

QJsonValue LC_JsonReader::value(const char* key) const {
    return m_object.value(QString::fromUtf8(key));
}

QJsonValue LC_JsonReader::required(const char* key) const {
    const QJsonValue v = value(key);
    if (v.isUndefined() || v.isNull()) {
        throw LC_ApiError(400, QStringLiteral("missing_param"), QStringLiteral("%1 is required").arg(fullPath(key)),
                          {{QStringLiteral("param"), fullPath(key)}});
    }
    return v;
}

void LC_JsonReader::allowOnly(const std::initializer_list<const char*> keys) const {
    QStringList allowed;
    allowed.reserve(static_cast<qsizetype>(keys.size()));
    for (const char* key : keys) {
        allowed << QString::fromUtf8(key);
    }
    for (auto it = m_object.constBegin(); it != m_object.constEnd(); ++it) {
        if (!allowed.contains(it.key())) {
            const QString field = m_path.isEmpty() ? it.key() : m_path + QLatin1Char('.') + it.key();
            QStringList sorted = allowed;
            sorted.sort();
            throw LC_ApiError(400, QStringLiteral("unknown_field"), QStringLiteral("unknown field %1").arg(field),
                              {{QStringLiteral("field"), field},
                               {QStringLiteral("allowed"), QJsonArray::fromStringList(sorted)}});
        }
    }
}

bool LC_JsonReader::has(const char* key) const {
    const QJsonValue v = value(key);
    return !v.isUndefined() && !v.isNull();
}

double LC_JsonReader::num(const char* key) const {
    const QJsonValue v = required(key);
    if (!v.isDouble() || !std::isfinite(v.toDouble())) {
        badParam(fullPath(key), QStringLiteral("%1 must be a number, not %2").arg(fullPath(key), describeType(v)));
    }
    return v.toDouble();
}

double LC_JsonReader::num(const char* key, const double def) const {
    return has(key) ? num(key) : def;
}

int LC_JsonReader::integer(const char* key, const int def, const int min, const int max) const {
    if (!has(key)) {
        return def;
    }
    const QJsonValue v = value(key);
    const double d = v.toDouble();
    if (!v.isDouble() || !std::isfinite(d) || std::floor(d) != d || d < min || d > max) {
        badParam(fullPath(key), QStringLiteral("%1 must be an integer from %2 to %3").arg(fullPath(key)).arg(min).arg(max),
                 {{QStringLiteral("min"), min}, {QStringLiteral("max"), max}});
    }
    return static_cast<int>(d);
}

bool LC_JsonReader::flag(const char* key, const bool def) const {
    if (!has(key)) {
        return def;
    }
    const QJsonValue v = value(key);
    if (!v.isBool()) {
        badParam(fullPath(key), QStringLiteral("%1 must be true or false, not %2").arg(fullPath(key), describeType(v)));
    }
    return v.toBool();
}

QString LC_JsonReader::str(const char* key) const {
    const QJsonValue v = required(key);
    if (!v.isString()) {
        badParam(fullPath(key), QStringLiteral("%1 must be a string, not %2").arg(fullPath(key), describeType(v)));
    }
    return v.toString();
}

QString LC_JsonReader::str(const char* key, const QString& def) const {
    return has(key) ? str(key) : def;
}

RS_Vector LC_JsonReader::point(const char* key) const {
    const QJsonValue v = required(key);
    const std::optional<RS_Vector> p = toPoint(v);
    if (!p) {
        badParam(fullPath(key), QStringLiteral("%1 must be a point [x, y] of finite numbers").arg(fullPath(key)));
    }
    return *p;
}

std::optional<RS_Vector> LC_JsonReader::optPoint(const char* key) const {
    if (!has(key)) {
        return std::nullopt;
    }
    return point(key);
}

QList<RS_Vector> LC_JsonReader::points(const char* key, const int minCount) const {
    const QJsonValue v = required(key);
    if (!v.isArray()) {
        badParam(fullPath(key), QStringLiteral("%1 must be an array of points [x, y]").arg(fullPath(key)));
    }
    const QJsonArray array = v.toArray();
    QList<RS_Vector> result;
    result.reserve(array.size());
    for (qsizetype i = 0; i < array.size(); ++i) {
        const std::optional<RS_Vector> p = toPoint(array.at(i));
        if (!p) {
            const QString itemPath = QStringLiteral("%1[%2]").arg(fullPath(key)).arg(i);
            badParam(itemPath, QStringLiteral("%1 must be a point [x, y] of finite numbers").arg(itemPath));
        }
        result << *p;
    }
    if (result.size() < minCount) {
        badParam(fullPath(key), QStringLiteral("%1 needs at least %2 points").arg(fullPath(key)).arg(minCount),
                 {{QStringLiteral("min"), minCount}});
    }
    return result;
}

LC_JsonReader LC_JsonReader::object(const char* key) const {
    const QJsonValue v = required(key);
    if (!v.isObject()) {
        badParam(fullPath(key), QStringLiteral("%1 must be an object, not %2").arg(fullPath(key), describeType(v)));
    }
    return LC_JsonReader(v.toObject(), fullPath(key));
}

QList<LC_JsonReader> LC_JsonReader::objects(const char* key) const {
    const QJsonValue v = required(key);
    if (!v.isArray()) {
        badParam(fullPath(key), QStringLiteral("%1 must be an array of objects").arg(fullPath(key)));
    }
    const QJsonArray array = v.toArray();
    QList<LC_JsonReader> result;
    result.reserve(array.size());
    for (qsizetype i = 0; i < array.size(); ++i) {
        const QString itemPath = QStringLiteral("%1[%2]").arg(fullPath(key)).arg(i);
        if (!array.at(i).isObject()) {
            badParam(itemPath, QStringLiteral("%1 must be an object, not %2").arg(itemPath, describeType(array.at(i))));
        }
        result << LC_JsonReader(array.at(i).toObject(), itemPath);
    }
    return result;
}

QString LC_JsonReader::enumOf(const char* key, const QStringList& allowed, const QString& def) const {
    if (!has(key)) {
        return def;
    }
    const QJsonValue v = value(key);
    if (!v.isString() || !allowed.contains(v.toString())) {
        badParam(fullPath(key), QStringLiteral("%1 must be one of: %2").arg(fullPath(key), allowed.join(QStringLiteral(", "))),
                 {{QStringLiteral("allowed"), QJsonArray::fromStringList(allowed)}});
    }
    return v.toString();
}

QJsonObject LC_JsonReader::raw() const {
    return m_object;
}

QJsonValue lcJsonPoint(const RS_Vector& v) {
    if (!v.valid || !std::isfinite(v.x) || !std::isfinite(v.y)) {
        return QJsonValue(QJsonValue::Null);
    }
    return QJsonArray{v.x, v.y};
}

QString lcEntityTypeName(const RS2::EntityType type) {
    for (const TypeName& row : TypeNames) {
        if (row.type == type) {
            return QString::fromLatin1(row.name);
        }
    }
    return QStringLiteral("other");
}

RS2::EntityType lcEntityTypeFromName(const QString& name) {
    for (const TypeName& row : TypeNames) {
        if (name == QLatin1String(row.name)) {
            return row.type;
        }
    }
    return RS2::EntityUnknown;
}
