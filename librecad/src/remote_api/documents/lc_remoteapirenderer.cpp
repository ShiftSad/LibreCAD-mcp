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

#include "lc_remoteapirenderer.h"

#include <algorithm>
#include <climits>
#include <cmath>

#include <QBuffer>
#include <QImage>
#include <QImageWriter>
#include <QJsonObject>
#include <QPageLayout>
#include <QPrinter>

#include "lc_imageexporter.h"
#include "lc_plot_settings.h"
#include "lc_printing.h"
#include "lc_remoteapitypes.h"
#include "rs_graphic.h"
#include "rs_math.h"
#include "rs_settings.h"

namespace {

/**
 * Restores the drawing's "modified" flag on scope exit when it was clear
 * before: a viewport zoom marks the drawing modified (ModifyOnViewChange), and
 * a picture of the drawing is no change to it.
 */
class ModifiedFlagGuard {
public:
    explicit ModifiedFlagGuard(RS_Graphic* graphic)
        : m_graphic(graphic), m_wasModified(graphic->isModified()) {
    }
    ~ModifiedFlagGuard() {
        if (!m_wasModified && m_graphic->isModified()) {
            m_graphic->setModified(false);
        }
    }
    ModifiedFlagGuard(const ModifiedFlagGuard&) = delete;
    ModifiedFlagGuard& operator=(const ModifiedFlagGuard&) = delete;

private:
    RS_Graphic* m_graphic;
    bool m_wasModified;
};

/** Extents of the visible entities, as zoomAuto() frames them; false for an empty drawing. */
bool visibleExtents(RS_Graphic* graphic, RS_Vector& min, RS_Vector& max) {
    graphic->calculateBorders();
    min = graphic->getMin();
    max = graphic->getMax();
    return min.valid && max.valid && max.x >= min.x && max.y >= min.y;
}

/**
 * The world window covering a whole picture of size in which content (world
 * coordinates) is centred and fitted inside margin pixels; scale receives the
 * pixels per drawing unit. Throws 400 bad_param when the scale leaves the range
 * a viewport can show.
 */
QRectF fitWindow(const QRectF& content, const QSize& size, const int margin, double& scale) {
    const double width = size.width();
    const double height = size.height();
    const double innerWidth = std::max(1.0, width - 2.0 * margin);
    const double innerHeight = std::max(1.0, height - 2.0 * margin);
    const double dx = content.width();
    const double dy = content.height();

    if (dx <= RS_TOLERANCE && dy <= RS_TOLERANCE) {
        scale = 1.0;   // a single point: show it at 1 px per unit
    }
    else if (dx <= RS_TOLERANCE) {
        scale = innerHeight / dy;
    }
    else if (dy <= RS_TOLERANCE) {
        scale = innerWidth / dx;
    }
    else {
        scale = std::min(innerWidth / dx, innerHeight / dy);
    }

    const QPointF center = content.center();
    const double halfWidth = width / (2.0 * scale);
    const double halfHeight = height / (2.0 * scale);
    const QRectF window(QPointF(center.x() - halfWidth, center.y() - halfHeight),
                        QPointF(center.x() + halfWidth, center.y() + halfHeight));

    // LC_GraphicViewport::zoomWindow() refuses (and leaves the view as it was)
    // when a corner is further than INT_MAX / 2 pixels from the origin.
    bool inRange = std::isfinite(scale) && scale > 0.0;
    for (const double coordinate : {window.left(), window.right(), window.top(), window.bottom()}) {
        inRange = inRange && std::abs(coordinate * scale) < INT_MAX / 4.0;
    }
    if (!inRange) {
        throw LC_ApiError(400, QStringLiteral("bad_param"),
                          QStringLiteral("the area to render is too small or too far from the origin"),
                          {{QStringLiteral("param"), QStringLiteral("window")}});
    }
    return window;
}

QByteArray encodePng(const QImage& image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return {};
    }
    QImageWriter writer(&buffer, "png");
    if (!writer.write(image)) {
        return {};
    }
    buffer.close();
    return bytes;
}

} // namespace

QByteArray LC_RemoteApiRenderer::renderPng(RS_Graphic* graphic, const Options& options, const QRectF& worldWindow,
                                           QRectF* outWindow, double* outScale, bool* outEmpty) {
    if (graphic == nullptr || options.size.isEmpty()) {
        return {};
    }
    ModifiedFlagGuard keepModified(graphic);

    RS_Vector min;
    RS_Vector max;
    const bool hasEntities = visibleExtents(graphic, min, max);
    if (outEmpty != nullptr) {
        *outEmpty = !hasEntities;
    }

    QRectF window;
    double scale = 1.0;
    if (worldWindow.isNull()) {
        if (!hasEntities) {
            // nothing to frame: a blank picture, centred on the origin at 1 px per unit
            QImage blank(options.size, QImage::Format_ARGB32_Premultiplied);
            blank.fill(options.backgroundBlack ? Qt::black : Qt::white);
            if (outWindow != nullptr) {
                *outWindow = QRectF(-options.size.width() / 2.0, -options.size.height() / 2.0,
                                    options.size.width(), options.size.height());
            }
            if (outScale != nullptr) {
                *outScale = 1.0;
            }
            return encodePng(blank);
        }
        window = fitWindow(QRectF(QPointF(min.x, min.y), QPointF(max.x, max.y)), options.size, options.marginPx, scale);
    }
    else {
        window = fitWindow(worldWindow.normalized(), options.size, options.marginPx, scale);
    }

    LC_ImageExporter::ExportOptions exportOptions;
    exportOptions.format = QStringLiteral("png");
    exportOptions.size = options.size;
    exportOptions.borders = QSize(0, 0);   // the margin is part of the window
    exportOptions.backgroundBlack = options.backgroundBlack;
    exportOptions.blackAndWhite = options.monochrome;
    exportOptions.window = window;

    QImage image;
    if (!LC_ImageExporter{}.renderToImage(graphic, exportOptions, image)) {
        return {};
    }
    if (outWindow != nullptr) {
        *outWindow = window;
    }
    if (outScale != nullptr) {
        *outScale = scale;
    }
    return encodePng(image);
}

bool LC_RemoteApiRenderer::exportImage(RS_Graphic* graphic, const QString& path, const QString& format,
                                       const Options& options) {
    if (graphic == nullptr) {
        return false;
    }
    ModifiedFlagGuard keepModified(graphic);

    LC_ImageExporter::ExportOptions exportOptions;
    exportOptions.fileName = path;
    exportOptions.format = format.toLower();
    exportOptions.size = options.size;
    exportOptions.borders = QSize(options.marginPx, options.marginPx);
    exportOptions.backgroundBlack = options.backgroundBlack;
    exportOptions.blackAndWhite = options.monochrome;
    return LC_ImageExporter{}.exportToImage(graphic, exportOptions);
}

bool LC_RemoteApiRenderer::exportPdf(RS_Graphic* graphic, const QString& path, const bool fitToPage,
                                     const bool monochrome, const bool scaleLineWidth, const int dpi,
                                     QString* error) {
    if (graphic == nullptr) {
        return false;
    }
    ModifiedFlagGuard keepModified(graphic);
    // Paper scale ($PSVPSCALE) and insertion base ($PINSBASE) live in the
    // drawing variables; fitToPage() changes both.
    const RS_VariableDict savedVariables = graphic->getVariableDictObject();

    LC_PlotSettings* plotSettings = graphic->getPlotSettings();
    if (fitToPage) {
        graphic->calculateBorders();
        // RS_Graphic::fitToPage() resets the paper to A4, also in the settings
        // for new drawings, when the drawing cannot be fitted: check first.
        const RS_Vector printArea = plotSettings->getPrintAreaSize(false);
        RS_Vector size = graphic->getSize();
        size.x = std::abs(size.x) < RS_TOLERANCE ? 10.0 : size.x;
        size.y = std::abs(size.y) < RS_TOLERANCE ? 10.0 : size.y;
        const double scale = std::min(printArea.x / size.x, printArea.y / size.y);
        if (std::isfinite(scale) && scale > 1.0e-10 && scale < RS_MAXDOUBLE) {
            graphic->fitToPage();
        }
    }

    // As LC_Printing::print() sets up the PDF printer, without its file dialog.
    QPrinter printer(QPrinter::HighResolution);
    printer.setFullPage(true);
    bool landscape = false;
    const RS2::PaperFormat paperFormat = plotSettings->getPaperFormat(&landscape);
    const QPageSize::PageSizeId paperSizeName = LC_Printing::rsToQtPaperFormat(paperFormat);
    const RS_Vector paperSize = plotSettings->getPaperSize();
    const auto printMargins = graphic->activeLayoutMargins();
    const QMarginsF paperMargins{printMargins[0], printMargins[1], printMargins[2], printMargins[3]};
    LC_Printing::setupPageLayout(printer, landscape, paperSizeName, paperSize, graphic->getUnit(), paperMargins);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setResolution(dpi);
    QPageLayout pdfLayout = printer.pageLayout();
    pdfLayout.setMinimumMargins({});
    printer.setPageLayout(pdfLayout);
    printer.setColorMode(monochrome ? QPrinter::GrayScale : QPrinter::Color);
    printer.setOutputFileName(path);

    const bool printed = LC_Printing::printGraphic(printer, *graphic, monochrome ? RS2::ModeBW : RS2::ModeAuto,
                                                   scaleLineWidth);

    graphic->setVariableDictObject(savedVariables);
    if (!printed && error != nullptr) {
        *error = QStringLiteral("cannot write the file %1").arg(path);
    }
    return printed;
}
