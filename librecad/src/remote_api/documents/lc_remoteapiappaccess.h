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

#ifndef LC_REMOTEAPIAPPACCESS_H
#define LC_REMOTEAPIAPPACCESS_H

#include <functional>

#include <QString>

#include "rs.h"

class QC_ApplicationWindow;
class QC_MDIWindow;
class QG_ActionHandler;
class QG_CommandWidget;

/**
 * Dialog-free document operations of the Remote API on the main window.
 *
 * A friend of QC_ApplicationWindow (librecad-mcp hook in its header): the
 * GUI's own slots ask the user (file dialogs, Save/Discard, error boxes), and
 * the members these operations need are protected. Each method mirrors the
 * GUI code it names; keep them in step with upstream (UPSTREAM_SYNC.md).
 * GUI thread only.
 */
class LC_RemoteApiAppAccess {
public:
    /**
     * New drawing from the default template, as slotFileNewFromDefaultTemplate().
     * Returns the new, active window. When the template cannot be read the
     * drawing starts empty and *templateFailed (when given) is set.
     */
    static QC_MDIWindow* newDocument(QC_ApplicationWindow* app, bool* templateFailed = nullptr);

    /**
     * Opens path as openFile(path, type) does, without its message boxes:
     * onImportError (see RS_FileIO::fileImport) answers import errors. Returns
     * the new, active window, or nullptr when the file could not be read; the
     * window made for it is then closed again.
     *
     * Mirrors QC_ApplicationWindow::openFile(); keep it in step.
     */
    static QC_MDIWindow* openDocument(QC_ApplicationWindow* app, const QString& path, RS2::FormatType type,
                                      std::function<bool(bool, const QString&)> onImportError);

    /** What doSave() does after a successful save: title, recent files, autosave timer. */
    static void afterSaved(QC_ApplicationWindow* app, QC_MDIWindow* window);

    /**
     * Closes window without asking (its unsaved changes are lost), as
     * closeWindow() does once the user chose "Don't Save".
     */
    static void close(QC_ApplicationWindow* app, QC_MDIWindow* window);

    /** Makes window the active document window. */
    static void activate(QC_ApplicationWindow* app, QC_MDIWindow* window);

    static QG_CommandWidget* commandWidget(QC_ApplicationWindow* app);
    static QG_ActionHandler* actionHandler(QC_ApplicationWindow* app);
};

#endif
