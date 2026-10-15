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

#ifndef LC_REMOTEAPIFORMATS_H
#define LC_REMOTEAPIFORMATS_H

// File formats of the Remote API (design section 2.5, /formats): the DXF and DWG
// versions LibreCAD can write, their wire names, aliases and labels, and the
// file types it can open. This is the one table to extend when LibreCAD learns
// a new version: add a row to the version table in the .cpp file.

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include "rs.h"

namespace LC_RemoteApiFormats {

/** One writable version of a drawing format. */
struct Version {
    QString format;           // "dxf" or "dwg"
    QString name;             // wire name: "R12", "2000", "2018", ...
    QString acadVersion;      // "AC1009" ... "AC1032"
    QString label;            // "AutoCAD 2018-2026 (AC1032)"
    RS2::FormatType type = RS2::FormatUnknown;
};

/** The writable versions of format ("dxf" or "dwg"), oldest first, that RS_FileIO::canExport() accepts. */
QList<Version> versions(const QString& format);

/** The version written when a request names none: "2007" for DXF, "2018" for DWG. */
QString defaultVersion(const QString& format);

/** Alternative names of the versions of format, e.g. {"2026": "2018", "AC1032": "2018"}. */
QHash<QString, QString> aliases(const QString& format);

/** Whether this build writes DWG (DWGSUPPORT). */
bool dwgSupported();

/**
 * The RS2::FormatType that writes format ("dxf" or "dwg") in version; an empty
 * version is the default one. Names are case-insensitive and aliases resolve.
 * Throws LC_ApiError: 422 unsupported_format (+allowed) for another format,
 * 501 dwg_unsupported for DWG in a build without DWGSUPPORT, and
 * 422 bad_param (+param "version", +allowed) for an unknown version.
 */
RS2::FormatType resolve(const QString& format, const QString& version);

/** The table row of a writable type, or nullptr (e.g. FormatUnknown, FormatLFF). */
const Version* find(RS2::FormatType type);

/**
 * The format of a document as the API reports it:
 * {"format": "dxf"|"dwg"|"other", "version": str|null, "label": str},
 * or null for RS2::FormatUnknown (a drawing read only in part, for example).
 */
QJsonValue describe(RS2::FormatType type);

/** File extensions LibreCAD opens, lower case with the dot: ".dxf", ".dwg", ... */
QStringList openExtensions();

/**
 * The format to read path with: RS_FileIO::detectFormat(), or the extension for
 * the types it does not detect (JWW, JWC). RS2::FormatUnknown if unsupported.
 */
RS2::FormatType openFormat(const QString& path);

/** Export formats: "dxf", "dwg" (with DWGSUPPORT), "pdf", "png", "svg". */
QStringList exportFormats();

/** The file extension of an export or save format, with the dot: ".dxf", ".pdf", ... */
QString extension(const QString& format);

/** The body of GET /api/v1/formats. */
QJsonObject toJson();

} // namespace LC_RemoteApiFormats

#endif
