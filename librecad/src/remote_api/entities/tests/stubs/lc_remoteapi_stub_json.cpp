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

// TEST-ONLY stand-in for core/lc_remoteapijson.cpp.
//
// The entities tests need LC_JsonReader and the type name table while the
// core component's implementation is not on the branch. entities.cmake adds
// this file to the test sources only while core/lc_remoteapijson.cpp does not
// exist, so it drops out by itself once the real one lands. It follows the
// contract documented in core/lc_remoteapijson.h and is never linked into the
// application.

#include <cmath>

#include <QJsonArray>

#include "lc_remoteapijson.h"
#include "lc_remoteapitypes.h"

namespace {

QString fullPath(const QString& path, const char* key) {
    return path.isEmpty() ? QString::fromLatin1(key) : path + QLatin1Char('.') + QLatin1String(key);
}

[[noreturn]] void badParam(const QString& param, const QString& detail, const QJsonArray& allowed = {}) {
    QJsonObject extra{{QStringLiteral("param"), param}};
    if (!allowed.isEmpty()) {
        extra.insert(QStringLiteral("allowed"), allowed);
    }
    throw LC_ApiError(400, QStringLiteral("bad_param"), detail, extra);
}

[[noreturn]] void missingParam(const QString& param) {
    throw LC_ApiError(400, QStringLiteral("missing_param"), QStringLiteral("'%1' is required").arg(param),
                      {{QStringLiteral("param"), param}});
}

bool isFiniteNumber(const QJsonValue& v) {
    return v.isDouble() && std::isfinite(v.toDouble());
}

RS_Vector toPoint(const QJsonValue& v, const QString& param) {
    const QJsonArray a = v.toArray();
    if (!v.isArray() || a.size() < 2 || a.size() > 3 || !isFiniteNumber(a.at(0)) || !isFiniteNumber(a.at(1)) ||
        (a.size() == 3 && !isFiniteNumber(a.at(2)))) {
        badParam(param, QStringLiteral("'%1' must be a point [x, y]").arg(param));
    }
    return {a.at(0).toDouble(), a.at(1).toDouble()};
}

} // namespace

LC_JsonReader::LC_JsonReader(QJsonObject o, QString path)
    : m_object(std::move(o))
    , m_path(std::move(path)) {
}

void LC_JsonReader::allowOnly(const std::initializer_list<const char*> keys) const {
    for (auto it = m_object.constBegin(); it != m_object.constEnd(); ++it) {
        bool known = false;
        for (const char* key : keys) {
            if (it.key() == QLatin1String(key)) {
                known = true;
                break;
            }
        }
        if (!known) {
            const QString field = m_path.isEmpty() ? it.key() : m_path + QLatin1Char('.') + it.key();
            throw LC_ApiError(400, QStringLiteral("unknown_field"), QStringLiteral("unknown field '%1'").arg(field),
                              {{QStringLiteral("field"), field}});
        }
    }
}

bool LC_JsonReader::has(const char* key) const {
    return m_object.contains(QLatin1String(key));
}

double LC_JsonReader::num(const char* key) const {
    const QJsonValue v = m_object.value(QLatin1String(key));
    if (v.isUndefined()) {
        missingParam(fullPath(m_path, key));
    }
    if (!isFiniteNumber(v)) {
        badParam(fullPath(m_path, key), QStringLiteral("'%1' must be a number").arg(fullPath(m_path, key)));
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
    const QJsonValue v = m_object.value(QLatin1String(key));
    const double d = v.toDouble();
    if (!isFiniteNumber(v) || d != std::floor(d) || d < min || d > max) {
        badParam(fullPath(m_path, key), QStringLiteral("'%1' must be an integer from %2 to %3").arg(fullPath(m_path, key)).arg(min).arg(max));
    }
    return static_cast<int>(d);
}

bool LC_JsonReader::flag(const char* key, const bool def) const {
    if (!has(key)) {
        return def;
    }
    const QJsonValue v = m_object.value(QLatin1String(key));
    if (!v.isBool()) {
        badParam(fullPath(m_path, key), QStringLiteral("'%1' must be true or false").arg(fullPath(m_path, key)));
    }
    return v.toBool();
}

QString LC_JsonReader::str(const char* key) const {
    const QJsonValue v = m_object.value(QLatin1String(key));
    if (v.isUndefined()) {
        missingParam(fullPath(m_path, key));
    }
    if (!v.isString()) {
        badParam(fullPath(m_path, key), QStringLiteral("'%1' must be a string").arg(fullPath(m_path, key)));
    }
    return v.toString();
}

QString LC_JsonReader::str(const char* key, const QString& def) const {
    return has(key) ? str(key) : def;
}

RS_Vector LC_JsonReader::point(const char* key) const {
    const QJsonValue v = m_object.value(QLatin1String(key));
    if (v.isUndefined()) {
        missingParam(fullPath(m_path, key));
    }
    return toPoint(v, fullPath(m_path, key));
}

std::optional<RS_Vector> LC_JsonReader::optPoint(const char* key) const {
    if (!has(key)) {
        return std::nullopt;
    }
    return point(key);
}

QList<RS_Vector> LC_JsonReader::points(const char* key, const int minCount) const {
    const QJsonValue v = m_object.value(QLatin1String(key));
    const QString param = fullPath(m_path, key);
    if (v.isUndefined()) {
        missingParam(param);
    }
    if (!v.isArray()) {
        badParam(param, QStringLiteral("'%1' must be an array of points").arg(param));
    }
    QList<RS_Vector> out;
    const QJsonArray a = v.toArray();
    for (int i = 0; i < a.size(); ++i) {
        out.append(toPoint(a.at(i), param + QLatin1Char('[') + QString::number(i) + QLatin1Char(']')));
    }
    if (out.size() < minCount) {
        badParam(param, QStringLiteral("'%1' needs at least %2 points").arg(param).arg(minCount));
    }
    return out;
}

LC_JsonReader LC_JsonReader::object(const char* key) const {
    const QJsonValue v = m_object.value(QLatin1String(key));
    const QString param = fullPath(m_path, key);
    if (v.isUndefined()) {
        missingParam(param);
    }
    if (!v.isObject()) {
        badParam(param, QStringLiteral("'%1' must be an object").arg(param));
    }
    return LC_JsonReader(v.toObject(), param);
}

QList<LC_JsonReader> LC_JsonReader::objects(const char* key) const {
    const QJsonValue v = m_object.value(QLatin1String(key));
    const QString param = fullPath(m_path, key);
    if (v.isUndefined()) {
        missingParam(param);
    }
    if (!v.isArray()) {
        badParam(param, QStringLiteral("'%1' must be an array of objects").arg(param));
    }
    QList<LC_JsonReader> out;
    const QJsonArray a = v.toArray();
    for (int i = 0; i < a.size(); ++i) {
        const QString itemPath = param + QLatin1Char('[') + QString::number(i) + QLatin1Char(']');
        if (!a.at(i).isObject()) {
            badParam(itemPath, QStringLiteral("'%1' must be an object").arg(itemPath));
        }
        out.append(LC_JsonReader(a.at(i).toObject(), itemPath));
    }
    return out;
}

QString LC_JsonReader::enumOf(const char* key, const QStringList& allowed, const QString& def) const {
    if (!has(key)) {
        return def;
    }
    const QJsonValue v = m_object.value(QLatin1String(key));
    if (!v.isString() || !allowed.contains(v.toString())) {
        badParam(fullPath(m_path, key), QStringLiteral("'%1' must be one of %2").arg(fullPath(m_path, key), allowed.join(QStringLiteral(", "))),
                 QJsonArray::fromStringList(allowed));
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
    switch (type) {
        case RS2::EntityLine:
            return QStringLiteral("line");
        case RS2::EntityCircle:
            return QStringLiteral("circle");
        case RS2::EntityArc:
            return QStringLiteral("arc");
        case RS2::EntityEllipse:
            return QStringLiteral("ellipse");
        case RS2::EntityPoint:
            return QStringLiteral("point");
        case RS2::EntityPolyline:
            return QStringLiteral("polyline");
        case RS2::EntitySpline:
        case RS2::EntitySplinePoints:
            return QStringLiteral("spline");
        case RS2::EntityText:
            return QStringLiteral("text");
        case RS2::EntityMText:
            return QStringLiteral("mtext");
        case RS2::EntityHatch:
            return QStringLiteral("hatch");
        case RS2::EntityInsert:
            return QStringLiteral("insert");
        case RS2::EntityDimAligned:
        case RS2::EntityDimLinear:
        case RS2::EntityDimRadial:
        case RS2::EntityDimDiametric:
        case RS2::EntityDimAngular:
        case RS2::EntityDimArc:
        case RS2::EntityDimOrdinate:
            return QStringLiteral("dimension");
        case RS2::EntityDimLeader:
            return QStringLiteral("leader");
        case RS2::EntityMLeader:
            return QStringLiteral("mleader");
        case RS2::EntityImage:
            return QStringLiteral("image");
        case RS2::EntitySolid:
            return QStringLiteral("solid");
        case RS2::EntityParabola:
            return QStringLiteral("parabola");
        case RS2::EntityHyperbola:
            return QStringLiteral("hyperbola");
        default:
            return QStringLiteral("other");
    }
}

RS2::EntityType lcEntityTypeFromName(const QString& name) {
    static const QList<std::pair<QString, RS2::EntityType>> table{
        {QStringLiteral("line"), RS2::EntityLine},         {QStringLiteral("circle"), RS2::EntityCircle},
        {QStringLiteral("arc"), RS2::EntityArc},           {QStringLiteral("ellipse"), RS2::EntityEllipse},
        {QStringLiteral("point"), RS2::EntityPoint},       {QStringLiteral("polyline"), RS2::EntityPolyline},
        {QStringLiteral("spline"), RS2::EntitySpline},     {QStringLiteral("text"), RS2::EntityText},
        {QStringLiteral("mtext"), RS2::EntityMText},       {QStringLiteral("hatch"), RS2::EntityHatch},
        {QStringLiteral("insert"), RS2::EntityInsert},     {QStringLiteral("dimension"), RS2::EntityDimLinear},
        {QStringLiteral("leader"), RS2::EntityDimLeader},  {QStringLiteral("mleader"), RS2::EntityMLeader},
        {QStringLiteral("image"), RS2::EntityImage},       {QStringLiteral("solid"), RS2::EntitySolid},
        {QStringLiteral("parabola"), RS2::EntityParabola}, {QStringLiteral("hyperbola"), RS2::EntityHyperbola}};
    for (const auto& entry : table) {
        if (entry.first == name) {
            return entry.second;
        }
    }
    return RS2::EntityUnknown;
}
