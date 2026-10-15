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

// Route group of the documents component (design section 2.4, owner C):
// formats, documents, drawing, render, view, undo/redo, command(s).
//
// Handlers run on the GUI thread (every route here has gui = true) and never
// open a dialog: open passes an import error callback, save and save_as use
// LC_RemoteApiDocumentsStorage, export writes through RS_FileIO,
// LC_ImageExporter and its own QPrinter, and close never asks.

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>

#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRectF>
#include <QTextBlock>
#include <QTextDocument>

#include "lc_graphicviewport.h"
#include "lc_remoteapiappaccess.h"
#include "lc_remoteapicontext.h"
#include "lc_remoteapidocumentsstorage.h"
#include "lc_remoteapiformats.h"
#include "lc_remoteapijson.h"
#include "lc_remoteapipathpolicy.h"
#include "lc_remoteapirenderer.h"
#include "lc_remoteapirouter.h"
#include "lc_remoteapiroutes.h"
#include "lc_remoteapitypes.h"
#include "lc_view.h"
#include "lc_viewslist.h"
#include "qc_applicationwindow.h"
#include "qc_mdiwindow.h"
#include "qg_commandwidget.h"
#include "qg_graphicview.h"
#include "rs_blocklist.h"
#include "rs_commands.h"
#include "rs_document.h"
#include "rs_entity.h"
#include "rs_fileio.h"
#include "rs_graphic.h"
#include "rs_layer.h"
#include "rs_layerlist.h"

namespace {

using Access = LC_RemoteApiPathPolicy::Access;

constexpr int DefaultTimeoutMs = 10000;
constexpr int FormatsTimeoutMs = 5000;
constexpr int RenderTimeoutMs = 30000;
constexpr int CommandTimeoutMs = 30000;
constexpr int FileTimeoutMs = 120000;   // open, save, save_as, export, new
constexpr int MaxUndoSteps = 1000;
constexpr int MaxCommandInputs = 1000;

// ---------------------------------------------------------------- helpers

LC_RouteOptions routeOptions(const char* capability, const char* summary, const int timeoutMs,
                             const bool needsDocument, const bool changesDocument,
                             const bool keepActiveAction = false) {
    LC_RouteOptions options;
    options.capability = QString::fromLatin1(capability);
    options.summary = QString::fromUtf8(summary);
    options.gui = true;
    options.timeoutMs = timeoutMs;
    options.needsDocument = needsDocument;
    options.changesDocument = changesDocument;
    options.keepActiveAction = keepActiveAction;
    return options;
}

QJsonArray toArray(const QStringList& list) {
    QJsonArray array;
    for (const QString& item : list) {
        array.append(item);
    }
    return array;
}

[[noreturn]] void badParam(const QString& param, const QString& detail, const QStringList& allowed = {}) {
    QJsonObject extra{{QStringLiteral("param"), param}};
    if (!allowed.isEmpty()) {
        extra.insert(QStringLiteral("allowed"), toArray(allowed));
    }
    throw LC_ApiError(400, QStringLiteral("bad_param"), detail, extra);
}

QString number(const double value) {
    return QString::number(value, 'g', 12);
}

QStringList splitReport(const QString& report) {
    return report.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

/** The application window of the request; every route here needs it. */
QC_ApplicationWindow* appWindow(const LC_ApiCall& call) {
    QC_ApplicationWindow* app = call.ctx.appWindow();
    if (app == nullptr) {
        throw LC_ApiError(503, QStringLiteral("shutting_down"), QStringLiteral("LibreCAD has no main window"));
    }
    return app;
}

// ---- query parameters (GET /render)

void allowQuery(const LC_ApiRequest& request, const QStringList& keys) {
    for (auto it = request.query.cbegin(); it != request.query.cend(); ++it) {
        if (!keys.contains(it.key())) {
            throw LC_ApiError(400, QStringLiteral("unknown_field"),
                              QStringLiteral("unknown query parameter '%1'").arg(it.key()),
                              {{QStringLiteral("field"), it.key()}, {QStringLiteral("allowed"), toArray(keys)}});
        }
    }
}

bool hasQuery(const LC_ApiRequest& request, const QString& key) {
    return request.query.contains(key);
}

QString queryValue(const LC_ApiRequest& request, const QString& key, const QString& def = {}) {
    const QList<QString> values = request.query.values(key);
    if (values.isEmpty()) {
        return def;
    }
    if (values.size() > 1) {
        badParam(key, QStringLiteral("'%1' is given more than once").arg(key));
    }
    return values.first().trimmed();
}

int queryInt(const LC_ApiRequest& request, const QString& key, const int def, const int min, const int max) {
    if (!hasQuery(request, key)) {
        return def;
    }
    bool ok = false;
    const int value = queryValue(request, key).toInt(&ok);
    if (!ok || value < min || value > max) {
        badParam(key, QStringLiteral("'%1' must be an integer from %2 to %3").arg(key).arg(min).arg(max));
    }
    return value;
}

bool queryBool(const LC_ApiRequest& request, const QString& key, const bool def) {
    if (!hasQuery(request, key)) {
        return def;
    }
    const QString value = queryValue(request, key).toLower();
    if (value == QLatin1String("true") || value == QLatin1String("1")) {
        return true;
    }
    if (value == QLatin1String("false") || value == QLatin1String("0")) {
        return false;
    }
    badParam(key, QStringLiteral("'%1' must be true or false").arg(key), {"true", "false"});
}

QString queryEnum(const LC_ApiRequest& request, const QString& key, const QStringList& allowed, const QString& def) {
    if (!hasQuery(request, key)) {
        return def;
    }
    const QString value = queryValue(request, key).toLower();
    if (!allowed.contains(value)) {
        badParam(key, QStringLiteral("'%1' must be one of: %2").arg(key, allowed.join(QStringLiteral(", "))), allowed);
    }
    return value;
}

RS_Vector queryPoint(const LC_ApiRequest& request, const QString& key) {
    if (!hasQuery(request, key)) {
        throw LC_ApiError(400, QStringLiteral("missing_param"), QStringLiteral("'%1' is required").arg(key),
                          {{QStringLiteral("param"), key}});
    }
    const QStringList parts = queryValue(request, key).split(QLatin1Char(','));
    bool okX = false;
    bool okY = false;
    const double x = parts.size() == 2 ? parts.at(0).trimmed().toDouble(&okX) : 0.0;
    const double y = parts.size() == 2 ? parts.at(1).trimmed().toDouble(&okY) : 0.0;
    if (!okX || !okY || !std::isfinite(x) || !std::isfinite(y)) {
        badParam(key, QStringLiteral("'%1' must be a point written x,y").arg(key));
    }
    return {x, y};
}

// ---- documents

int liveEntityCount(RS_Document* document) {
    int count = 0;
    for (RS_Entity* entity : *document) {
        if (entity != nullptr && !entity->isDeleted()) {
            ++count;
        }
    }
    return count;
}

QString documentName(const QC_MDIWindow* window) {
    return LC_RemoteApiContext::documentName(window);
}

/** The Doc schema of section 2.3. */
QJsonObject documentJson(const LC_RemoteApiContext& ctx, QC_MDIWindow* window) {
    const QString path = window->getFileName();
    RS_Graphic* graphic = window->getGraphic();
    return QJsonObject{
        {QStringLiteral("id"), static_cast<qint64>(window->getId())},
        {QStringLiteral("name"), documentName(window)},
        {QStringLiteral("path"), path.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(path)},
        {QStringLiteral("modified"), window->isModified()},
        {QStringLiteral("active"), ctx.activeDocumentWindow() == window},
        {QStringLiteral("format"),
         graphic != nullptr ? LC_RemoteApiFormats::describe(graphic->getFormatType()) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("entity_count"), liveEntityCount(window->getDocument())}};
}

/** 409 file_open_in_other_window when a document other than self is stored in path. */
void refuseFileOfOtherDocument(const LC_RemoteApiContext& ctx, const QC_MDIWindow* self, const QString& path) {
    for (QC_MDIWindow* window : ctx.documentWindows()) {
        if (window != self && LC_RemoteApiPathPolicy::sameFile(window->getFileName(), path)) {
            throw LC_ApiError(409, QStringLiteral("file_open_in_other_window"),
                              QStringLiteral("another open drawing (id %1) is stored in this file").arg(window->getId()),
                              {{QStringLiteral("path"), path},
                               {QStringLiteral("document"), static_cast<qint64>(window->getId())}});
        }
    }
}

/** {"format": "dxf", "version": "2018", "label": ...} fields of a written file. */
void addFormatFields(QJsonObject& result, const RS2::FormatType type) {
    if (const LC_RemoteApiFormats::Version* version = LC_RemoteApiFormats::find(type)) {
        result.insert(QStringLiteral("format"), version->format);
        result.insert(QStringLiteral("version"), version->name);
        result.insert(QStringLiteral("label"), version->label);
    }
}

// ---- views

QRectF viewWindow(const LC_GraphicViewport* viewport) {
    const double width = viewport->getWidth();
    const double height = viewport->getHeight();
    const RS_Vector corners[] = {viewport->toWorldFromUi(0, 0), viewport->toWorldFromUi(width, 0),
                                 viewport->toWorldFromUi(0, height), viewport->toWorldFromUi(width, height)};
    double minX = corners[0].x;
    double minY = corners[0].y;
    double maxX = minX;
    double maxY = minY;
    for (const RS_Vector& corner : corners) {
        minX = std::min(minX, corner.x);
        minY = std::min(minY, corner.y);
        maxX = std::max(maxX, corner.x);
        maxY = std::max(maxY, corner.y);
    }
    return {QPointF(minX, minY), QPointF(maxX, maxY)};
}

/** The View schema of section 2.3. */
QJsonObject viewJson(const LC_GraphicViewport* viewport) {
    const QRectF window = viewWindow(viewport);
    return QJsonObject{
        {QStringLiteral("window"),
         QJsonObject{{QStringLiteral("min"), lcJsonPoint(RS_Vector(window.left(), window.top()))},
                     {QStringLiteral("max"), lcJsonPoint(RS_Vector(window.right(), window.bottom()))}}},
        {QStringLiteral("center"), lcJsonPoint(RS_Vector(window.center().x(), window.center().y()))},
        {QStringLiteral("size_px"), QJsonArray{viewport->getWidth(), viewport->getHeight()}},
        {QStringLiteral("px_per_unit"), viewport->getFactor().x}};
}

QString unitName(const RS2::Unit unit) {
    static const char* const names[] = {
        "none", "inch", "foot", "mile", "mm", "cm", "m", "km", "microinch", "mil", "yard",
        "angstrom", "nm", "micron", "dm", "dam", "hm", "gm", "astro", "lightyear", "parsec"};
    const int index = static_cast<int>(unit);
    return index >= 0 && index < static_cast<int>(std::size(names)) ? QString::fromLatin1(names[index])
                                                                     : QStringLiteral("none");
}

QJsonValue boxJson(const RS_Vector& min, const RS_Vector& max) {
    if (!min.valid || !max.valid || max.x < min.x || max.y < min.y) {
        return QJsonValue::Null;
    }
    return QJsonObject{{QStringLiteral("min"), lcJsonPoint(min)}, {QStringLiteral("max"), lcJsonPoint(max)}};
}

/** {"can_undo": bool, "can_redo": bool} */
QJsonObject undoStateJson(const RS_Document* document) {
    bool canUndo = false;
    bool canRedo = false;
    document->collectUndoState(canUndo, canRedo);
    return QJsonObject{{QStringLiteral("can_undo"), canUndo}, {QStringLiteral("can_redo"), canRedo}};
}

// ---------------------------------------------------------------- handlers

LC_ApiResponse getFormats(LC_ApiCall& /*call*/) {
    return LC_ApiResponse::ok(LC_RemoteApiFormats::toJson());
}

LC_ApiResponse listDocuments(LC_ApiCall& call) {
    QJsonArray documents;
    for (QC_MDIWindow* window : call.ctx.documentWindows()) {
        documents.append(documentJson(call.ctx, window));
    }
    QC_MDIWindow* active = call.ctx.activeDocumentWindow();
    return LC_ApiResponse::ok(
        {{QStringLiteral("documents"), documents},
         {QStringLiteral("active"),
          active != nullptr ? QJsonValue(static_cast<qint64>(active->getId())) : QJsonValue(QJsonValue::Null)}});
}

LC_ApiResponse newDocument(LC_ApiCall& call) {
    const LC_JsonReader& body = call.body();
    body.allowOnly({"activate"});
    const bool activate = body.flag("activate", true);
    QC_ApplicationWindow* app = appWindow(call);

    QC_MDIWindow* previous = call.ctx.activeDocumentWindow();
    bool templateFailed = false;
    QC_MDIWindow* window = LC_RemoteApiAppAccess::newDocument(app, &templateFailed);
    if (window == nullptr) {
        throw LC_ApiError(500, QStringLiteral("internal"), QStringLiteral("LibreCAD could not create a drawing"));
    }
    // Without a template file, RS_Graphic::onLoadingCompleted() adds the
    // default dimension style, which leaves a drawing nobody touched marked
    // modified (one made from a template is marked saved). Start it clean, so
    // that closing it needs no discard_changes.
    if (RS_Graphic* graphic = window->getGraphic()) {
        graphic->setModified(false);
    }
    if (templateFailed) {
        call.warnings.append(QStringLiteral("template_not_loaded: the default template could not be read; "
                                            "the drawing starts empty"));
    }
    if (!activate && previous != nullptr) {
        LC_RemoteApiAppAccess::activate(app, previous);
    }
    call.audit = QStringLiteral("[MCP] created a new drawing (%1)").arg(documentName(window));
    return LC_ApiResponse::ok({{QStringLiteral("document"), documentJson(call.ctx, window)}}, 201);
}

LC_ApiResponse openDocument(LC_ApiCall& call) {
    const LC_JsonReader& body = call.body();
    body.allowOnly({"path", "accept_partial"});
    const QString requested = body.str("path");
    const bool acceptPartial = body.flag("accept_partial", false);
    QC_ApplicationWindow* app = appWindow(call);

    const QString path = LC_RemoteApiPathPolicy(call.ctx.config()).check(requested, Access::Read);

    for (QC_MDIWindow* window : call.ctx.documentWindows()) {
        if (LC_RemoteApiPathPolicy::sameFile(window->getFileName(), path)) {
            LC_RemoteApiAppAccess::activate(app, window);
            return LC_ApiResponse::ok({{QStringLiteral("document"), documentJson(call.ctx, window)},
                                       {QStringLiteral("already_open"), true}});
        }
    }

    if (QFileInfo(path).suffix().compare(QLatin1String("dwg"), Qt::CaseInsensitive) == 0
        && !LC_RemoteApiFormats::dwgSupported()) {
        throw LC_ApiError(501, QStringLiteral("dwg_unsupported"),
                          QStringLiteral("this LibreCAD build cannot read DWG files"), {{QStringLiteral("path"), path}});
    }
    const RS2::FormatType type = LC_RemoteApiFormats::openFormat(path);
    if (type == RS2::FormatUnknown) {
        throw LC_ApiError(422, QStringLiteral("unsupported_format"),
                          QStringLiteral("LibreCAD cannot open this type of file"),
                          {{QStringLiteral("path"), path},
                           {QStringLiteral("allowed"), toArray(LC_RemoteApiFormats::openExtensions())}});
    }

    // RS_FileIO calls this instead of showing its error boxes. Its answer is
    // the import result: true opens a partly read drawing anyway.
    bool importFailed = false;
    bool partial = false;
    QString importError;
    const auto onImportError = [&](const bool hasEntities, const QString& error) {
        importFailed = true;
        partial = hasEntities;
        importError = error;
        return hasEntities && acceptPartial;
    };
    QC_MDIWindow* window = LC_RemoteApiAppAccess::openDocument(app, path, type, onImportError);
    if (window == nullptr) {
        if (importFailed && partial) {
            throw LC_ApiError(409, QStringLiteral("partial_import"), importError,
                              {{QStringLiteral("path"), path},
                               {QStringLiteral("hint"),
                                QStringLiteral("only part of the file could be read; pass \"accept_partial\": true "
                                               "to open what was read")}});
        }
        throw LC_ApiError(422, QStringLiteral("import_failed"),
                          importError.isEmpty() ? QStringLiteral("LibreCAD could not read the file") : importError,
                          {{QStringLiteral("path"), path}});
    }
    if (importFailed && partial) {
        // as the GUI's "open anyway": Save must not write over a file that was not read completely
        if (RS_Graphic* graphic = window->getGraphic()) {
            graphic->setFormatType(RS2::FormatUnknown);
        }
        call.warnings.append(QStringLiteral("partial_import: %1").arg(importError));
    }
    call.audit = QStringLiteral("[MCP] opened %1").arg(path);
    return LC_ApiResponse::ok({{QStringLiteral("document"), documentJson(call.ctx, window)}}, 201);
}

LC_ApiResponse activateDocument(LC_ApiCall& call) {
    call.body().allowOnly({});
    LC_ResolvedDoc& doc = call.doc();
    LC_RemoteApiAppAccess::activate(appWindow(call), doc.window);
    return LC_ApiResponse::ok({{QStringLiteral("document"), documentJson(call.ctx, doc.window)}});
}

LC_ApiResponse saveDocument(LC_ApiCall& call) {
    call.body().allowOnly({});
    LC_ResolvedDoc& doc = call.doc();
    QC_ApplicationWindow* app = appWindow(call);
    RS_Graphic* graphic = doc.graphic;
    const QString fileName = graphic->getFilename();

    if (fileName.isEmpty()) {
        throw LC_ApiError(409, QStringLiteral("document_unnamed"), QStringLiteral("the drawing has no file yet"),
                          {{QStringLiteral("hint"), QStringLiteral("use save_as")}});
    }
    const RS2::FormatType type = graphic->getFormatType();
    if (graphic->isDxfRecoverySourceFile(fileName) || !RS_FileIO::instance()->canExport(type)) {
        throw LC_ApiError(409, QStringLiteral("cannot_save_in_place"),
                          QStringLiteral("LibreCAD cannot write this drawing back to its file in its format"),
                          {{QStringLiteral("format"), LC_RemoteApiFormats::describe(type)},
                           {QStringLiteral("path"), fileName},
                           {QStringLiteral("hint"), QStringLiteral("use save_as with a DXF or DWG version")}});
    }
    LC_RemoteApiPathPolicy(call.ctx.config()).check(fileName, Access::WriteInPlace);

    const QFileInfo info(fileName);
    if (info.exists() && !info.isWritable()) {
        throw LC_ApiError(500, QStringLiteral("save_failed"), QStringLiteral("the file is read-only"),
                          {{QStringLiteral("path"), fileName}});
    }
    const QDateTime lastSave = graphic->getLastSaveTime();
    if (info.exists() && lastSave.isValid() && info.lastModified() != lastSave) {
        throw LC_ApiError(500, QStringLiteral("save_failed"),
                          QStringLiteral("the file was changed on disk by another program since LibreCAD read it"),
                          {{QStringLiteral("path"), fileName},
                           {QStringLiteral("hint"), QStringLiteral("use save_as to keep both versions")}});
    }

    LC_RemoteApiDocumentsStorage storage;   // no target: a Save As question is answered with cancel
    bool cancelled = false;
    const bool saved = storage.saveDocument(doc.document, doc.view, cancelled);
    doc.window->setWindowModified(doc.document->isModified());
    if (cancelled || storage.wasAsked()) {
        throw LC_ApiError(409, QStringLiteral("cannot_save_in_place"),
                          QStringLiteral("LibreCAD would ask for a new file name"),
                          {{QStringLiteral("format"), LC_RemoteApiFormats::describe(type)},
                           {QStringLiteral("hint"), QStringLiteral("use save_as")}});
    }
    const QStringList leftOut = splitReport(RS_FileIO::instance()->lastExportReport());
    if (!saved) {
        throw LC_ApiError(500, QStringLiteral("save_failed"),
                          leftOut.isEmpty() ? QStringLiteral("LibreCAD could not write the file") : leftOut.join("; "),
                          {{QStringLiteral("path"), fileName}});
    }
    LC_RemoteApiAppAccess::afterSaved(app, doc.window);

    call.audit = QStringLiteral("[MCP] saved %1").arg(fileName);
    QJsonObject result{{QStringLiteral("document"), documentJson(call.ctx, doc.window)},
                       {QStringLiteral("path"), fileName},
                       {QStringLiteral("left_out"), toArray(leftOut)}};
    addFormatFields(result, graphic->getFormatType());
    return LC_ApiResponse::ok(result);
}

LC_ApiResponse saveDocumentAs(LC_ApiCall& call) {
    const LC_JsonReader& body = call.body();
    body.allowOnly({"path", "format", "version", "overwrite"});
    const QString requested = body.str("path");
    const QString format = body.str("format", QStringLiteral("dxf")).trimmed().toLower();
    const QString version = body.str("version", QString());
    const bool overwrite = body.flag("overwrite", false);
    LC_ResolvedDoc& doc = call.doc();
    QC_ApplicationWindow* app = appWindow(call);

    const RS2::FormatType type = LC_RemoteApiFormats::resolve(format, version);
    const QString path = LC_RemoteApiPathPolicy(call.ctx.config())
                             .check(requested, Access::Write, {LC_RemoteApiFormats::extension(format)}, overwrite);
    refuseFileOfOtherDocument(call.ctx, doc.window, path);
    if (doc.graphic->isDxfRecoverySourceFile(path)) {
        throw LC_ApiError(403, QStringLiteral("path_not_allowed"),
                          QStringLiteral("the original legacy DXF is protected; save the recovered drawing to another file"),
                          {{QStringLiteral("path"), path}});
    }
    const bool replacing = QFileInfo::exists(path);

    LC_RemoteApiDocumentsStorage storage;
    storage.setTarget(path, type);
    bool cancelled = false;
    const bool saved = storage.saveDocumentAs(doc.document, doc.view, cancelled);
    doc.window->setWindowModified(doc.document->isModified());
    const QStringList leftOut = splitReport(RS_FileIO::instance()->lastExportReport());
    if (!saved || cancelled) {
        throw LC_ApiError(500, QStringLiteral("save_failed"),
                          leftOut.isEmpty() ? QStringLiteral("LibreCAD could not write the file") : leftOut.join("; "),
                          {{QStringLiteral("path"), path}});
    }
    LC_RemoteApiAppAccess::afterSaved(app, doc.window);

    const LC_RemoteApiFormats::Version* written = LC_RemoteApiFormats::find(type);
    call.audit = QStringLiteral("[MCP] saved as %1 (%2)").arg(path, written != nullptr ? written->label : format);
    if (replacing) {
        call.audit += QStringLiteral(", replacing the existing file");
        call.forceAudit = true;
    }
    QJsonObject result{{QStringLiteral("document"), documentJson(call.ctx, doc.window)},
                       {QStringLiteral("path"), path},
                       {QStringLiteral("left_out"), toArray(leftOut)}};
    addFormatFields(result, type);
    return LC_ApiResponse::ok(result);
}

LC_ApiResponse exportDocument(LC_ApiCall& call) {
    const LC_JsonReader& body = call.body();
    body.allowOnly({"path", "format", "version", "overwrite", "width", "height", "margin_px", "background",
                    "monochrome", "fit_to_page"});
    const QString requested = body.str("path");
    const QString format = body.str("format").trimmed().toLower();
    const bool overwrite = body.flag("overwrite", false);
    LC_ResolvedDoc& doc = call.doc();

    if (format == QLatin1String("dwg") && !LC_RemoteApiFormats::dwgSupported()) {
        throw LC_ApiError(501, QStringLiteral("dwg_unsupported"), QStringLiteral("this LibreCAD build cannot write DWG"));
    }
    const QStringList formats = LC_RemoteApiFormats::exportFormats();
    if (!formats.contains(format)) {
        throw LC_ApiError(422, QStringLiteral("unsupported_format"),
                          QStringLiteral("format must be one of: %1").arg(formats.join(QStringLiteral(", "))),
                          {{QStringLiteral("param"), QStringLiteral("format")},
                           {QStringLiteral("allowed"), toArray(formats)}});
    }
    const bool drawing = format == QLatin1String("dxf") || format == QLatin1String("dwg");
    if (!drawing && body.has("version")) {
        badParam(QStringLiteral("version"), QStringLiteral("only DXF and DWG exports have a version"));
    }
    const RS2::FormatType type = drawing ? LC_RemoteApiFormats::resolve(format, body.str("version", QString()))
                                         : RS2::FormatUnknown;

    LC_RemoteApiRenderer::Options picture;
    picture.size = QSize(body.integer("width", 2000, LC_RemoteApiRenderer::MinSide, LC_RemoteApiRenderer::MaxSide),
                         body.integer("height", 1000, LC_RemoteApiRenderer::MinSide, LC_RemoteApiRenderer::MaxSide));
    picture.marginPx = body.integer("margin_px", 16, 0, LC_RemoteApiRenderer::MaxSide);
    if (2 * picture.marginPx >= std::min(picture.size.width(), picture.size.height())) {
        badParam(QStringLiteral("margin_px"), QStringLiteral("the margin leaves no room for the drawing"));
    }
    picture.backgroundBlack = body.enumOf("background", {"white", "black"}, QStringLiteral("white"))
                              == QLatin1String("black");
    picture.monochrome = body.flag("monochrome", false);
    const bool fitToPage = body.flag("fit_to_page", true);

    const QString path = LC_RemoteApiPathPolicy(call.ctx.config())
                             .check(requested, Access::Write, {LC_RemoteApiFormats::extension(format)}, overwrite);
    refuseFileOfOtherDocument(call.ctx, doc.window, path);
    if (LC_RemoteApiPathPolicy::sameFile(doc.window->getFileName(), path)) {
        throw LC_ApiError(409, QStringLiteral("file_open_in_other_window"),
                          QStringLiteral("the drawing itself is stored in this file; use save or save_as"),
                          {{QStringLiteral("path"), path}});
    }
    const bool replacing = QFileInfo::exists(path);

    RS_Graphic* graphic = doc.graphic;
    bool written = false;
    QString error;
    QStringList leftOut;
    if (drawing) {
        // a copy: the drawing keeps its file name, format and modified flag
        const bool wasModified = graphic->isModified();
        graphic->prepareForSave();   // as a save does, so the copy is what Save As would write
        written = RS_FileIO::instance()->fileExport(*graphic, path, type);
        leftOut = splitReport(RS_FileIO::instance()->lastExportReport());
        if (!wasModified && graphic->isModified()) {
            graphic->setModified(false);
        }
        if (!written) {
            error = leftOut.join(QStringLiteral("; "));
        }
    }
    else if (format == QLatin1String("pdf")) {
        written = LC_RemoteApiRenderer::exportPdf(graphic, path, fitToPage, picture.monochrome,
                                                  doc.view != nullptr && doc.view->getLineWidthScaling(), 600, &error);
    }
    else {
        written = LC_RemoteApiRenderer::exportImage(graphic, path, format, picture);
    }
    if (!written) {
        throw LC_ApiError(500, QStringLiteral("export_failed"),
                          error.isEmpty() ? QStringLiteral("LibreCAD could not write the file") : error,
                          {{QStringLiteral("path"), path}});
    }

    call.audit = QStringLiteral("[MCP] exported a %1 copy to %2").arg(format.toUpper(), path);
    if (replacing) {
        call.audit += QStringLiteral(", replacing the existing file");
        call.forceAudit = true;
    }
    QJsonObject result{{QStringLiteral("path"), path},
                       {QStringLiteral("format"), format},
                       {QStringLiteral("bytes"), static_cast<qint64>(QFileInfo(path).size())},
                       {QStringLiteral("left_out"), toArray(leftOut)}};
    if (drawing) {
        addFormatFields(result, type);
    }
    return LC_ApiResponse::ok(result);
}

LC_ApiResponse closeDocument(LC_ApiCall& call) {
    const LC_JsonReader& body = call.body();
    body.allowOnly({"discard_changes"});
    const bool discard = body.flag("discard_changes", false);
    LC_ResolvedDoc& doc = call.doc();
    QC_ApplicationWindow* app = appWindow(call);

    const bool modified = doc.window->isModified();
    if (modified && !discard) {
        throw LC_ApiError(409, QStringLiteral("unsaved_changes"), QStringLiteral("the drawing has unsaved changes"),
                          {{QStringLiteral("hint"),
                            QStringLiteral("save it first (save or save_as), or close with \"discard_changes\": true")}});
    }
    const QString name = documentName(doc.window);
    LC_RemoteApiAppAccess::close(app, doc.window);

    if (modified) {
        call.audit = QStringLiteral("[MCP] closed '%1' and discarded its unsaved changes").arg(name);
        call.forceAudit = true;
    }
    else {
        call.audit = QStringLiteral("[MCP] closed '%1'").arg(name);
    }
    return LC_ApiResponse::ok({{QStringLiteral("closed"), true}, {QStringLiteral("discarded_changes"), modified}});
}

LC_ApiResponse drawingInfo(LC_ApiCall& call) {
    LC_ResolvedDoc& doc = call.doc();
    RS_Graphic* graphic = doc.graphic;
    RS_Document* document = doc.document;
    const RS2::Unit unit = graphic->getUnit();

    // visible entities, as zoom auto frames them
    document->calculateBorders();
    const QJsonValue extents = boxJson(document->getMin(), document->getMax());

    // every live top-level entity, also on frozen layers, by type
    RS_Vector allMin(false);
    RS_Vector allMax(false);
    QJsonObject byType;
    int total = 0;
    for (RS_Entity* entity : *document) {
        if (entity == nullptr || entity->isDeleted()) {
            continue;
        }
        ++total;
        const QString typeName = lcEntityTypeName(entity->rtti());
        byType.insert(typeName, byType.value(typeName).toInt() + 1);
        const RS_Vector min = entity->getMin();
        const RS_Vector max = entity->getMax();
        if (!min.valid || !max.valid || !std::isfinite(min.x) || !std::isfinite(max.x)) {
            continue;
        }
        if (!allMin.valid) {
            allMin = min;
            allMax = max;
        }
        else {
            allMin = RS_Vector::minimum(allMin, min);
            allMax = RS_Vector::maximum(allMax, max);
        }
    }

    const RS_Layer* current = graphic->getActiveLayer();
    QJsonObject result{
        {QStringLiteral("document"), documentJson(call.ctx, doc.window)},
        {QStringLiteral("units"), unitName(unit)},
        {QStringLiteral("unit_code"), static_cast<int>(unit)},
        {QStringLiteral("extents"), extents},
        {QStringLiteral("extents_all"), boxJson(allMin, allMax)},
        {QStringLiteral("format"), LC_RemoteApiFormats::describe(graphic->getFormatType())},
        {QStringLiteral("counts"), QJsonObject{{QStringLiteral("total"), total}, {QStringLiteral("by_type"), byType}}},
        {QStringLiteral("layers"), static_cast<int>(graphic->getLayerList()->count())},
        {QStringLiteral("blocks"), static_cast<int>(graphic->getBlockList()->count())},
        {QStringLiteral("current_layer"),
         current != nullptr ? QJsonValue(current->getName()) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("undo"), undoStateJson(document)},
        {QStringLiteral("variables"),
         QJsonObject{{QStringLiteral("$INSUNITS"), static_cast<int>(unit)},
                     {QStringLiteral("$LTSCALE"), graphic->getVariableDouble(QStringLiteral("$LTSCALE"), 1.0)},
                     {QStringLiteral("$DIMSCALE"), graphic->getVariableDouble(QStringLiteral("$DIMSCALE"), 1.0)}}},
        // v1 addresses model space only (design section 7, item 4)
        {QStringLiteral("paper_space"), QStringLiteral("not_supported")}};
    return LC_ApiResponse::ok(result);
}

LC_ApiResponse render(LC_ApiCall& call) {
    const LC_ApiRequest& request = call.request;
    allowQuery(request, {"doc", "mode", "min", "max", "width", "height", "margin_px", "background", "monochrome"});
    LC_ResolvedDoc& doc = call.doc();

    const QString mode = queryEnum(request, QStringLiteral("mode"), {"extents", "view", "window"},
                                   QStringLiteral("extents"));
    LC_RemoteApiRenderer::Options options;
    options.size = QSize(queryInt(request, QStringLiteral("width"), 1024, LC_RemoteApiRenderer::MinSide,
                                  LC_RemoteApiRenderer::MaxSide),
                         queryInt(request, QStringLiteral("height"), 768, LC_RemoteApiRenderer::MinSide,
                                  LC_RemoteApiRenderer::MaxSide));
    options.marginPx = queryInt(request, QStringLiteral("margin_px"), 16, 0, LC_RemoteApiRenderer::MaxSide);
    if (2 * options.marginPx >= std::min(options.size.width(), options.size.height())) {
        badParam(QStringLiteral("margin_px"), QStringLiteral("the margin leaves no room for the drawing"));
    }
    options.backgroundBlack = queryEnum(request, QStringLiteral("background"), {"white", "black"},
                                        QStringLiteral("white")) == QLatin1String("black");
    options.monochrome = queryBool(request, QStringLiteral("monochrome"), false);

    QRectF window;   // null: the extents
    if (mode == QLatin1String("window")) {
        const RS_Vector min = queryPoint(request, QStringLiteral("min"));
        const RS_Vector max = queryPoint(request, QStringLiteral("max"));
        window = QRectF(QPointF(min.x, min.y), QPointF(max.x, max.y)).normalized();
        if (window.width() <= 0.0 && window.height() <= 0.0) {
            badParam(QStringLiteral("max"), QStringLiteral("the window has no area"));
        }
    }
    else {
        for (const char* key : {"min", "max"}) {
            if (hasQuery(request, QString::fromLatin1(key))) {
                badParam(QString::fromLatin1(key), QStringLiteral("min and max are for mode=window"));
            }
        }
        if (mode == QLatin1String("view")) {
            window = viewWindow(doc.viewport);
            if (window.width() <= 0.0 || window.height() <= 0.0) {
                badParam(QStringLiteral("mode"), QStringLiteral("the drawing's view has no size (window not shown)"));
            }
        }
    }

    QRectF rendered;
    double scale = 0.0;
    bool empty = false;
    const QByteArray png = LC_RemoteApiRenderer::renderPng(doc.graphic, options, window, &rendered, &scale, &empty);
    if (png.isEmpty()) {
        throw LC_ApiError(500, QStringLiteral("internal"), QStringLiteral("rendering the drawing failed"));
    }
    LC_ApiResponse response = LC_ApiResponse::bytes(png, QByteArrayLiteral("image/png"));
    response.extraHeaders.append({QByteArrayLiteral("X-LibreCAD-Window"),
                                  QStringLiteral("%1,%2,%3,%4")
                                      .arg(number(rendered.left()), number(rendered.top()), number(rendered.right()),
                                           number(rendered.bottom()))
                                      .toLatin1()});
    response.extraHeaders.append({QByteArrayLiteral("X-LibreCAD-Scale"), number(scale).toLatin1()});
    if (empty) {
        response.extraHeaders.append({QByteArrayLiteral("X-LibreCAD-Empty"), QByteArrayLiteral("1")});
    }
    return response;
}

LC_ApiResponse getView(LC_ApiCall& call) {
    return LC_ApiResponse::ok(viewJson(call.doc().viewport));
}

LC_ApiResponse setView(LC_ApiCall& call) {
    const LC_JsonReader& body = call.body();
    body.allowOnly({"action", "min", "max", "factor", "center", "offset", "name"});
    if (!body.has("action")) {
        throw LC_ApiError(400, QStringLiteral("missing_param"), QStringLiteral("'action' is required"),
                          {{QStringLiteral("param"), QStringLiteral("action")}});
    }
    const QStringList actions{"extents", "window", "zoom_in", "zoom_out", "pan", "previous", "named"};
    const QString action = body.enumOf("action", actions, QStringLiteral("extents"));

    // the fields each action reads; any other given field is a mistake
    const QHash<QString, QStringList> fields{{"extents", {}},
                                             {"window", {"min", "max"}},
                                             {"zoom_in", {"factor", "center"}},
                                             {"zoom_out", {"factor", "center"}},
                                             {"pan", {"center", "offset"}},
                                             {"previous", {}},
                                             {"named", {"name"}}};
    for (const char* field : {"min", "max", "factor", "center", "offset", "name"}) {
        if (body.has(field) && !fields.value(action).contains(QString::fromLatin1(field))) {
            badParam(QString::fromLatin1(field),
                     QStringLiteral("'%1' is not used by action '%2'").arg(QString::fromLatin1(field), action));
        }
    }

    LC_ResolvedDoc& doc = call.doc();
    LC_GraphicViewport* viewport = doc.viewport;
    if (action == QLatin1String("extents")) {
        viewport->zoomAuto(false);   // as the Zoom Auto action
    }
    else if (action == QLatin1String("window")) {
        const RS_Vector min = body.point("min");
        const RS_Vector max = body.point("max");
        if (std::abs(max.x - min.x) <= RS_TOLERANCE && std::abs(max.y - min.y) <= RS_TOLERANCE) {
            badParam(QStringLiteral("max"), QStringLiteral("the window has no area"));
        }
        // zoomWindow() takes UCS coordinates, as RS_ActionZoomWindow converts them
        viewport->zoomWindow(viewport->toUCS(min), viewport->toUCS(max), true);
    }
    else if (action == QLatin1String("zoom_in") || action == QLatin1String("zoom_out")) {
        const double factor = body.num("factor", 1.5);
        if (!(factor > 1.0) || factor > 1000.0) {
            badParam(QStringLiteral("factor"), QStringLiteral("'factor' must be greater than 1 and at most 1000"));
        }
        const std::optional<RS_Vector> center = body.optPoint("center");
        const RS_Vector ucsCenter = center.has_value() ? viewport->toUCS(*center) : RS_Vector(false);
        if (action == QLatin1String("zoom_in")) {
            viewport->zoomIn(factor, ucsCenter);
        }
        else {
            viewport->zoomOut(factor, ucsCenter);
        }
    }
    else if (action == QLatin1String("pan")) {
        RS_Vector ucsDelta;
        if (body.has("offset") == body.has("center")) {
            badParam(QStringLiteral("offset"), QStringLiteral("pan takes either 'offset' or 'center'"));
        }
        if (body.has("offset")) {
            ucsDelta = viewport->toUCSDelta(body.point("offset"));
        }
        else {
            const QRectF window = viewWindow(viewport);
            const RS_Vector currentCenter = viewport->toUCS(RS_Vector(window.center().x(), window.center().y()));
            ucsDelta = viewport->toUCS(body.point("center")) - currentCenter;
        }
        // zoomPan() moves the picture by pixels: the visible window moves the other way
        const double factor = viewport->getFactor().x;
        viewport->zoomPan(static_cast<int>(std::lround(-ucsDelta.x * factor)),
                          static_cast<int>(std::lround(ucsDelta.y * factor)));
    }
    else if (action == QLatin1String("previous")) {
        viewport->zoomPrevious();
    }
    else {   // named
        const QString name = body.str("name");
        LC_View* view = doc.graphic->getViewList()->find(name);
        if (view == nullptr) {
            throw LC_ApiError(404, QStringLiteral("unknown_view"), QStringLiteral("no named view '%1'").arg(name),
                              {{QStringLiteral("name"), name}});
        }
        if (view->isForPaperView()) {
            badParam(QStringLiteral("name"), QStringLiteral("paper-space views are not supported"));
        }
        // the Named Views widget does this for the active drawing, but shows a box for paper-space views
        viewport->restoreView(view);
    }
    return LC_ApiResponse::ok(viewJson(viewport));
}

LC_ApiResponse undoRedo(LC_ApiCall& call, const bool undo) {
    const LC_JsonReader& body = call.body();
    body.allowOnly({"steps"});
    const int steps = body.integer("steps", 1, 1, MaxUndoSteps);
    LC_ResolvedDoc& doc = call.doc();

    int done = 0;
    while (done < steps && (undo ? doc.document->undo() : doc.document->redo())) {
        ++done;
    }
    if (done > 0) {
        // as LC_ActionEditUndoRedo::trigger()
        doc.graphic->addBlockNotification();
        doc.graphic->setModified(true);
        doc.document->updateInserts();
        doc.view->redraw(RS2::RedrawDrawing);
        call.audit = undo ? QStringLiteral("[MCP] undid %1 step(s)").arg(done)
                          : QStringLiteral("[MCP] redid %1 step(s)").arg(done);
    }
    if (done < steps) {
        call.warnings.append(undo ? QStringLiteral("nothing more to undo") : QStringLiteral("nothing more to redo"));
    }
    QJsonObject result = undoStateJson(doc.document);
    result.insert(undo ? QStringLiteral("undone") : QStringLiteral("redone"), done);
    result.insert(QStringLiteral("layers_and_blocks_not_undone"), true);
    return LC_ApiResponse::ok(result);
}

/** The index of the history block where the next appended line goes. */
int historyEnd(const QTextDocument* history) {
    // QTextEdit::append() writes into the single empty block of an empty document
    return history->isEmpty() ? 0 : history->blockCount();
}

LC_ApiResponse runCommand(LC_ApiCall& call) {
    if (!call.ctx.config().allowCommands) {
        throw LC_ApiError(403, QStringLiteral("commands_disabled"), QStringLiteral("running commands is turned off"),
                          {{QStringLiteral("hint"),
                            QStringLiteral("enable 'Allow running command-line commands' in Preferences > Remote API")}});
    }
    const LC_JsonReader& body = call.body();
    body.allowOnly({"command", "inputs"});
    QStringList lines{body.str("command")};
    const QJsonValue inputs = body.raw().value(QStringLiteral("inputs"));
    if (!inputs.isUndefined() && !inputs.isNull()) {
        if (!inputs.isArray()) {
            badParam(QStringLiteral("inputs"), QStringLiteral("'inputs' must be a list of strings"));
        }
        const QJsonArray array = inputs.toArray();
        if (array.size() > MaxCommandInputs) {
            badParam(QStringLiteral("inputs"), QStringLiteral("at most %1 inputs").arg(MaxCommandInputs));
        }
        for (qsizetype i = 0; i < array.size(); ++i) {
            if (!array.at(i).isString()) {
                badParam(QStringLiteral("inputs[%1]").arg(i), QStringLiteral("each input must be a string"));
            }
            lines.append(array.at(i).toString());
        }
    }
    for (qsizetype i = 0; i < lines.size(); ++i) {
        // QG_CommandWidget::handleCommand() reads the first character of the text
        if (lines.at(i).simplified().isEmpty()) {
            badParam(i == 0 ? QStringLiteral("command") : QStringLiteral("inputs[%1]").arg(i - 1),
                     QStringLiteral("a command or input must not be empty"));
        }
    }

    LC_ResolvedDoc& doc = call.doc();
    QC_ApplicationWindow* app = appWindow(call);
    // the command line works on the active drawing
    LC_RemoteApiAppAccess::activate(app, doc.window);
    QG_CommandWidget* commandWidget = LC_RemoteApiAppAccess::commandWidget(app);
    if (commandWidget == nullptr) {
        throw LC_ApiError(500, QStringLiteral("internal"), QStringLiteral("LibreCAD has no command line"));
    }
    const QTextDocument* history = commandWidget->teHistory->document();
    const int firstLine = historyEnd(history);

    QJsonArray executed;
    QJsonArray unknown;
    for (const QString& line : std::as_const(lines)) {
        const int before = historyEnd(history);
        commandWidget->handleCommand(line);   // as if typed: echoed, then run
        executed.append(line);
        const QString unknownMessage = QG_CommandWidget::tr("Unknown command: %1").arg(line.simplified());
        for (int block = before; block < history->blockCount(); ++block) {
            if (history->findBlockByNumber(block).text() == unknownMessage) {
                unknown.append(line);
                break;
            }
        }
    }

    QJsonArray output;
    for (int block = firstLine; block < history->blockCount(); ++block) {
        output.append(history->findBlockByNumber(block).text());
    }
    const QString activeAction = doc.view->getCurrentActionName();
    call.audit = QStringLiteral("[MCP] ran command: %1").arg(lines.join(QStringLiteral(" | ")).left(200));
    return LC_ApiResponse::ok(
        {{QStringLiteral("executed"), executed},
         {QStringLiteral("output"), output},
         {QStringLiteral("prompt"), commandWidget->lCommand->text()},
         {QStringLiteral("active_action"),
          activeAction.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(activeAction)},
         {QStringLiteral("unknown_commands"), unknown}});
}

LC_ApiResponse listCommands(LC_ApiCall& /*call*/) {
    QStringList commands = RS_COMMANDS->complete(QString());
    commands.removeDuplicates();
    commands.sort(Qt::CaseInsensitive);
    return LC_ApiResponse::ok({{QStringLiteral("commands"), toArray(commands)}});
}

/** handler, after refusing every query parameter but doc (as the entity routes do). */
LC_ApiHandler onlyDocQuery(LC_ApiHandler handler) {
    return [handler = std::move(handler)](LC_ApiCall& call) {
        allowQuery(call.request, {"doc"});
        return handler(call);
    };
}

} // namespace

void lcRegisterDocumentRoutes(LC_RemoteApiRouter& router) {
    router.add("GET", QStringLiteral("/api/v1/formats"),
               routeOptions("documents", "File types LibreCAD opens, and the DXF/DWG versions it saves and exports",
                            FormatsTimeoutMs, false, false),
               onlyDocQuery(getFormats));
    router.add("GET", QStringLiteral("/api/v1/documents"),
               routeOptions("documents", "Open drawings", DefaultTimeoutMs, false, false), onlyDocQuery(listDocuments));
    router.add("POST", QStringLiteral("/api/v1/documents/new"),
               routeOptions("documents", "New drawing from the default template", FileTimeoutMs, false, true),
               onlyDocQuery(newDocument));
    router.add("POST", QStringLiteral("/api/v1/documents/open"),
               routeOptions("documents", "Open a drawing file (DXF, DWG, LFF, CXF, JWW, SHP)", FileTimeoutMs, false,
                            true),
               onlyDocQuery(openDocument));
    router.add("POST", QStringLiteral("/api/v1/documents/activate"),
               routeOptions("documents", "Make a drawing the active window", DefaultTimeoutMs, true, false),
               onlyDocQuery(activateDocument));
    router.add("POST", QStringLiteral("/api/v1/documents/save"),
               routeOptions("documents", "Save a drawing to its file, in its format", FileTimeoutMs, true, true),
               onlyDocQuery(saveDocument));
    router.add("POST", QStringLiteral("/api/v1/documents/save_as"),
               routeOptions("documents", "Save a drawing under a new name and format", FileTimeoutMs, true, true),
               onlyDocQuery(saveDocumentAs));
    router.add("POST", QStringLiteral("/api/v1/documents/export"),
               routeOptions("export", "Write a copy of a drawing as DXF, DWG, PDF, PNG or SVG", FileTimeoutMs, true,
                            false),
               onlyDocQuery(exportDocument));
    router.add("POST", QStringLiteral("/api/v1/documents/close"),
               routeOptions("documents", "Close a drawing; never asks", DefaultTimeoutMs, true, true), onlyDocQuery(closeDocument));
    router.add("GET", QStringLiteral("/api/v1/drawing"),
               routeOptions("drawing", "Units, extents, format, entity counts, layers, blocks and undo state",
                            DefaultTimeoutMs, true, false),
               onlyDocQuery(drawingInfo));
    router.add("GET", QStringLiteral("/api/v1/render"),
               routeOptions("render", "PNG picture of a drawing (the user's view is not changed)", RenderTimeoutMs,
                            true, false),
               render);
    router.add("GET", QStringLiteral("/api/v1/view"),
               routeOptions("view", "The visible window of the user's view", DefaultTimeoutMs, true, false), onlyDocQuery(getView));
    router.add("POST", QStringLiteral("/api/v1/view"),
               routeOptions("view", "Zoom or pan the user's view", DefaultTimeoutMs, true, false), onlyDocQuery(setView));
    router.add("POST", QStringLiteral("/api/v1/undo"),
               routeOptions("undo", "Undo entity changes (layer and block-list changes are not undoable)",
                            DefaultTimeoutMs, true, true),
               onlyDocQuery([](LC_ApiCall& call) { return undoRedo(call, true); }));
    router.add("POST", QStringLiteral("/api/v1/redo"),
               routeOptions("undo", "Redo entity changes", DefaultTimeoutMs, true, true),
               onlyDocQuery([](LC_ApiCall& call) { return undoRedo(call, false); }));
    router.add("POST", QStringLiteral("/api/v1/command"),
               routeOptions("command", "Type command-line commands and inputs", CommandTimeoutMs, true, true, true),
               onlyDocQuery(runCommand));
    router.add("GET", QStringLiteral("/api/v1/commands"),
               routeOptions("command", "Command names the command line accepts", DefaultTimeoutMs, false, false),
               onlyDocQuery(listCommands));
}
