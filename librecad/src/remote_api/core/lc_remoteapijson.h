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

#ifndef LC_REMOTEAPIJSON_H
#define LC_REMOTEAPIJSON_H

// Strict JSON input reading and shared JSON writers of the Remote API.
// Pinned interface (implemented in lc_remoteapijson.cpp by the core component).

#include <initializer_list>
#include <optional>

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include "rs.h"
#include "rs_vector.h"

/**
 * Strict reader over one JSON object of a request body.
 *
 * Every failure throws LC_ApiError (400) whose "param"/"field" names the full
 * path of the value, e.g. "entities[2].radius":
 * - a missing required key gives missing_param;
 * - a value of the wrong type or out of range gives bad_param (+ "allowed"
 *   for enumOf);
 * - a key outside allowOnly() gives unknown_field.
 * Points are [x, y]; a third value is accepted and ignored.
 */
class LC_JsonReader {                                       // strict, path-aware errors ("entities[2].radius")
public:
    LC_JsonReader(QJsonObject o, QString path = {});
    void allowOnly(std::initializer_list<const char*> keys) const;          // 400 unknown_field
    bool has(const char*) const;
    double num(const char*) const; double num(const char*, double def) const;
    int integer(const char*, int def, int min, int max) const;
    bool flag(const char*, bool def) const;
    QString str(const char*) const; QString str(const char*, const QString& def) const;
    RS_Vector point(const char*) const;  std::optional<RS_Vector> optPoint(const char*) const;
    QList<RS_Vector> points(const char*, int minCount) const;
    LC_JsonReader object(const char*) const; QList<LC_JsonReader> objects(const char*) const;
    QString enumOf(const char*, const QStringList& allowed, const QString& def) const; // 400 bad_param + allowed
    QJsonObject raw() const;

private:
    QString fullPath(const char* key) const;   // "entities[2].radius"
    QJsonValue value(const char* key) const;
    QJsonValue required(const char* key) const; // 400 missing_param when absent or null

    QJsonObject m_object;
    QString m_path;                            // "" for the body itself, "entities[2]" for a nested object
};

QJsonValue lcJsonPoint(const RS_Vector&);                   // [x,y] or null
// Wire type names of section 2.3 ("line", "circle", ..., "dimension", "other").
// lcEntityTypeFromName returns RS2::EntityUnknown for a name it does not know.
QString lcEntityTypeName(RS2::EntityType);  RS2::EntityType lcEntityTypeFromName(const QString&);

#endif
