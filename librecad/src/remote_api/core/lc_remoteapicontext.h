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

#ifndef LC_REMOTEAPICONTEXT_H
#define LC_REMOTEAPICONTEXT_H

// Application-side services for route handlers (GUI thread only).
// Pinned interface (implemented in lc_remoteapicontext.cpp by the core component).

#include <QList>
#include <QString>

#include "lc_remoteapiconfig.h"
#include "lc_remoteapitypes.h"

class LC_GraphicViewport;
class QC_ApplicationWindow;
class QC_MDIWindow;
class QG_GraphicView;
class RS_Document;
class RS_Graphic;

/**
 * A document window addressed by ?doc=<id> (QC_MDIWindow::getId()), with the
 * objects a handler works on. Print-preview windows resolve to their parent.
 */
struct LC_ResolvedDoc {
    unsigned id = 0;
    QC_MDIWindow* window = nullptr;
    RS_Document* document = nullptr;
    RS_Graphic* graphic = nullptr;
    QG_GraphicView* view = nullptr;
    LC_GraphicViewport* viewport = nullptr;
};

class LC_RemoteApiContext {
public:
    explicit LC_RemoteApiContext(QC_ApplicationWindow*);
    QC_ApplicationWindow* appWindow() const;
    LC_RemoteApiConfig config() const;                      // re-read from settings per request
    QList<QC_MDIWindow*> documentWindows() const;           // top-level, non-preview
    QC_MDIWindow* activeDocumentWindow() const;             // preview -> parent
    // ?doc= or the active document. Throws LC_ApiError: 409 no_document,
    // 404 unknown_document, 400 bad_param (doc is not an id).
    LC_ResolvedDoc resolve(const LC_ApiRequest&) const;     // ?doc= or active; throws
    LC_ResolvedDoc resolve(QC_MDIWindow*) const;
    QString cancelActiveAction(const LC_ResolvedDoc&) const; // getCurrentActionName + killAllActions; "" if idle
    void notify(const QString& line, bool force = false) const; // command-history line when Notify is on, or always with force

private:
    QC_ApplicationWindow* m_appWindow = nullptr;
};

#endif
