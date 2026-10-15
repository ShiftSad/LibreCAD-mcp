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

#ifndef LC_REMOTEAPIRENDERER_H
#define LC_REMOTEAPIRENDERER_H

// Pictures of a drawing for the Remote API, without dialogs: a PNG in memory
// (GET /render), and PNG, SVG and PDF files (POST /documents/export).
//
// Everything renders on a private viewport, so the user's view never changes,
// and the drawing's "modified" flag is the same afterwards: LibreCAD marks a
// drawing modified when a viewport zooms (setting "ModifyOnViewChange"), and
// fitting a PDF to its page changes the plot settings, both of which are
// undone here.

#include <QByteArray>
#include <QRectF>
#include <QSize>
#include <QString>

class RS_Graphic;

class LC_RemoteApiRenderer {
public:
    struct Options {
        QSize size{1024, 768};        // pixels
        int marginPx = 16;            // free border around the drawing, pixels
        bool backgroundBlack = false;
        bool monochrome = false;      // black on white (white on black)
    };

    /** Smallest and largest side of a rendered picture, pixels (design section 1.6). */
    static constexpr int MinSide = 64;
    static constexpr int MaxSide = 4096;

    /**
     * Renders graphic into a PNG of options.size.
     *
     * worldWindow is the area to show, in world coordinates; a null rectangle
     * shows the extents of the visible entities. The area is centred and fitted
     * inside the margin, keeping the aspect ratio, so the picture usually shows
     * more than asked for: outWindow receives the world area the whole picture
     * covers (x/y of the bottom-left corner, width, height) and outScale its
     * pixels per drawing unit. An empty drawing gives a blank picture and
     * *outEmpty = true (outWindow is then centred on the origin at 1 px/unit).
     * Returns an empty array when rendering fails.
     */
    static QByteArray renderPng(RS_Graphic* graphic, const Options& options, const QRectF& worldWindow,
                                QRectF* outWindow, double* outScale, bool* outEmpty = nullptr);

    /** Writes the whole drawing to path as "png" or "svg", as File > Export does. */
    static bool exportImage(RS_Graphic* graphic, const QString& path, const QString& format, const Options& options);

    /**
     * Writes graphic to path as PDF with its plot settings (paper, margins,
     * scale, pages), as File > Export to PDF does but without the file dialog.
     * With fitToPage the drawing is first fitted to the page (RS_Graphic::fitToPage);
     * the plot settings and the modified flag are restored afterwards.
     * On failure *error (when given) says why.
     */
    static bool exportPdf(RS_Graphic* graphic, const QString& path, bool fitToPage, bool monochrome,
                          bool scaleLineWidth = false, int dpi = 600, QString* error = nullptr);
};

#endif
