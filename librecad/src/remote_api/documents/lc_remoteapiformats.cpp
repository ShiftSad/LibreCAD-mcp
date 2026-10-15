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

#include "lc_remoteapiformats.h"

#include <QFileInfo>
#include <QJsonArray>

#include "lc_remoteapitypes.h"
#include "rs_fileio.h"

namespace {

/**
 * Every DXF and DWG version LibreCAD writes. A new version (for example a
 * future DWG format) is one more row here; /formats, save_as and export pick
 * it up. Rows of one format are listed oldest first.
 *
 * The console converters have the same version names (LC_Console's
 * dwgFormatForVersion), but they live in the executable, not in librecad_lib.
 */
struct Row {
    const char* format;
    const char* name;
    const char* acadVersion;
    const char* label;
    RS2::FormatType type;
};

const QList<Row>& rows() {
    static const QList<Row> table{
        {"dxf", "R12", "AC1009", "DXF R12 (AC1009)", RS2::FormatDXFRW12},
        {"dxf", "R14", "AC1014", "DXF R14 (AC1014)", RS2::FormatDXFRW14},
        {"dxf", "2000", "AC1015", "DXF 2000 (AC1015)", RS2::FormatDXFRW2000},
        {"dxf", "2004", "AC1018", "DXF 2004 (AC1018)", RS2::FormatDXFRW2004},
        {"dxf", "2007", "AC1021", "DXF 2007 (AC1021)", RS2::FormatDXFRW},
        {"dxf", "2010", "AC1024", "DXF 2010 (AC1024)", RS2::FormatDXFRW2010},
        {"dxf", "2013", "AC1027", "DXF 2013 (AC1027)", RS2::FormatDXFRW2013},
        {"dxf", "2018", "AC1032", "DXF 2018-2026 (AC1032)", RS2::FormatDXFRW2018},
#ifdef DWGSUPPORT
        {"dwg", "2000", "AC1015", "AutoCAD 2000 (AC1015)", RS2::FormatDWG},
        {"dwg", "2004", "AC1018", "AutoCAD 2004 (AC1018)", RS2::FormatDWG2004},
        {"dwg", "2007", "AC1021", "AutoCAD 2007 (AC1021)", RS2::FormatDWG2007},
        {"dwg", "2010", "AC1024", "AutoCAD 2010 (AC1024)", RS2::FormatDWG2010},
        {"dwg", "2013", "AC1027", "AutoCAD 2013 (AC1027)", RS2::FormatDWG2013},
        // AutoCAD 2019 through 2026 still save in the 2018 format.
        {"dwg", "2018", "AC1032", "AutoCAD 2018-2026 (AC1032)", RS2::FormatDWG2018},
#endif
    };
    return table;
}

/** Formats LibreCAD reads but does not write as drawings; reported as "other". */
QString otherLabel(const RS2::FormatType type) {
    switch (type) {
        case RS2::FormatDXF1:
            return QStringLiteral("QCad 1 DXF");
        case RS2::FormatLFF:
            return QStringLiteral("LibreCAD font (LFF)");
        case RS2::FormatCXF:
            return QStringLiteral("QCAD font (CXF)");
        case RS2::FormatJWW:
            return QStringLiteral("Jw_cad drawing (JWW)");
        case RS2::FormatJWC:
            return QStringLiteral("Jw_cad drawing (JWC)");
        case RS2::FormatSHP:
            return QStringLiteral("ESRI Shapefile (SHP)");
        default:
            return {};
    }
}

LC_RemoteApiFormats::Version toVersion(const Row& row) {
    return {QString::fromLatin1(row.format), QString::fromLatin1(row.name), QString::fromLatin1(row.acadVersion),
            QString::fromUtf8(row.label), row.type};
}

QJsonArray toArray(const QStringList& list) {
    QJsonArray array;
    for (const QString& item : list) {
        array.append(item);
    }
    return array;
}

QStringList drawingFormats() {
    return LC_RemoteApiFormats::dwgSupported() ? QStringList{"dxf", "dwg"} : QStringList{"dxf"};
}

} // namespace

namespace LC_RemoteApiFormats {

bool dwgSupported() {
#ifdef DWGSUPPORT
    return true;
#else
    return false;
#endif
}

QList<Version> versions(const QString& format) {
    QList<Version> result;
    const QString wanted = format.toLower();
    for (const Row& row : rows()) {
        if (wanted == QLatin1String(row.format) && RS_FileIO::instance()->canExport(row.type)) {
            result.append(toVersion(row));
        }
    }
    return result;
}

QString defaultVersion(const QString& format) {
    const QString wanted = format.toLower();
    if (wanted == QLatin1String("dxf")) {
        return QStringLiteral("2007");   // as the Save dialog: DXF 2007 stays the default
    }
    if (wanted == QLatin1String("dwg")) {
        return QStringLiteral("2018");
    }
    return {};
}

QHash<QString, QString> aliases(const QString& format) {
    QHash<QString, QString> result;
    for (const Version& version : versions(format)) {
        result.insert(version.acadVersion, version.name);
        // "R2000" ... spelled like the R12/R14 names
        if (!version.name.startsWith(QLatin1Char('R'))) {
            result.insert(QLatin1Char('R') + version.name, version.name);
        }
        if (version.acadVersion == QLatin1String("AC1032")) {
            // AutoCAD 2019 through 2026 write AC1032: name the newest release too
            for (int year = 2019; year <= 2026; ++year) {
                result.insert(QString::number(year), version.name);
            }
        }
    }
    return result;
}

RS2::FormatType resolve(const QString& format, const QString& version) {
    const QString name = format.trimmed().toLower();
    if (name == QLatin1String("dwg") && !dwgSupported()) {
        throw LC_ApiError(501, QStringLiteral("dwg_unsupported"),
                          QStringLiteral("this LibreCAD build cannot write DWG files"));
    }
    if (!drawingFormats().contains(name)) {
        throw LC_ApiError(422, QStringLiteral("unsupported_format"),
                          QStringLiteral("format must be one of: %1").arg(drawingFormats().join(QStringLiteral(", "))),
                          {{QStringLiteral("param"), QStringLiteral("format")},
                           {QStringLiteral("allowed"), toArray(drawingFormats())}});
    }

    const QList<Version> available = versions(name);
    QString wanted = version.trimmed();
    if (wanted.isEmpty()) {
        wanted = defaultVersion(name);
    }
    const QHash<QString, QString> alternativeNames = aliases(name);
    for (auto it = alternativeNames.cbegin(); it != alternativeNames.cend(); ++it) {
        if (it.key().compare(wanted, Qt::CaseInsensitive) == 0) {
            wanted = it.value();
            break;
        }
    }
    QStringList allowed;
    for (const Version& candidate : available) {
        if (candidate.name.compare(wanted, Qt::CaseInsensitive) == 0) {
            return candidate.type;
        }
        allowed.append(candidate.name);
    }
    throw LC_ApiError(422, QStringLiteral("bad_param"),
                      QStringLiteral("unknown %1 version '%2'").arg(name.toUpper(), version),
                      {{QStringLiteral("param"), QStringLiteral("version")},
                       {QStringLiteral("allowed"), toArray(allowed)}});
}

const Version* find(const RS2::FormatType type) {
    static const QList<Version> all = [] {
        QList<Version> list;
        for (const Row& row : rows()) {
            list.append(toVersion(row));
        }
        return list;
    }();
    for (const Version& version : all) {
        if (version.type == type) {
            return &version;
        }
    }
    return nullptr;
}

QJsonValue describe(const RS2::FormatType type) {
    if (type == RS2::FormatUnknown) {
        return QJsonValue::Null;
    }
    if (const Version* version = find(type)) {
        return QJsonObject{{QStringLiteral("format"), version->format},
                           {QStringLiteral("version"), version->name},
                           {QStringLiteral("label"), version->label}};
    }
    QString label = otherLabel(type);
    if (label.isEmpty()) {
        label = QStringLiteral("other");
    }
    return QJsonObject{{QStringLiteral("format"), QStringLiteral("other")},
                       {QStringLiteral("version"), QJsonValue::Null},
                       {QStringLiteral("label"), label}};
}

QStringList openExtensions() {
    QStringList list{".dxf"};
    if (dwgSupported()) {
        list << ".dwg";
    }
    list << ".lff" << ".cxf" << ".jww" << ".jwc" << ".shp";
    return list;
}

RS2::FormatType openFormat(const QString& path) {
    const RS2::FormatType detected = RS_FileIO::detectFormat(path);
    if (detected != RS2::FormatUnknown) {
        return detected;
    }
    // RS_FileIO::detectFormat() knows these only from the file dialog's filter
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("jww")) {
        return RS2::FormatJWW;
    }
    if (suffix == QLatin1String("jwc")) {
        return RS2::FormatJWC;
    }
    return RS2::FormatUnknown;
}

QStringList exportFormats() {
    QStringList list = drawingFormats();
    list << "pdf" << "png" << "svg";
    return list;
}

QString extension(const QString& format) {
    return QLatin1Char('.') + format.toLower();
}

QJsonObject toJson() {
    QJsonArray save;
    for (const QString& format : drawingFormats()) {
        QJsonArray names;
        QJsonObject labels;
        for (const Version& version : versions(format)) {
            names.append(version.name);
            labels.insert(version.name, version.label);
        }
        QJsonObject aliasObject;
        const QHash<QString, QString> alternativeNames = aliases(format);
        for (auto it = alternativeNames.cbegin(); it != alternativeNames.cend(); ++it) {
            aliasObject.insert(it.key(), it.value());
        }
        save.append(QJsonObject{{QStringLiteral("format"), format},
                                {QStringLiteral("versions"), names},
                                {QStringLiteral("default"), defaultVersion(format)},
                                {QStringLiteral("labels"), labels},
                                {QStringLiteral("aliases"), aliasObject},
                                {QStringLiteral("extension"), extension(format)}});
    }
    return QJsonObject{{QStringLiteral("open"), toArray(openExtensions())},
                       {QStringLiteral("save"), save},
                       {QStringLiteral("export"), toArray(exportFormats())}};
}

} // namespace LC_RemoteApiFormats
