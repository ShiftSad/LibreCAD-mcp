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

#ifndef LC_REMOTEAPIDOCUMENTSSTORAGE_H
#define LC_REMOTEAPIDOCUMENTSSTORAGE_H

#include "lc_documentsstorage.h"

/**
 * LC_DocumentsStorage without its Save As dialog, for the Remote API.
 *
 * Saving goes through the same code as the GUI (backups, autosave file, the
 * protected legacy DXF, markSaved). Where the GUI asks for a file name, this
 * class answers with the target set by setTarget(), or cancels when none is
 * set: no dialog can appear. A cancelled save reports cancelled = true, which
 * the routes turn into 409 document_unnamed or cannot_save_in_place.
 */
class LC_RemoteApiDocumentsStorage : public LC_DocumentsStorage {
public:
    LC_RemoteApiDocumentsStorage() = default;

    /** The answer to the next Save As question: path, written as type. */
    void setTarget(const QString& path, RS2::FormatType type);
    /** Whether the storage asked for a file name (the GUI would have shown a dialog). */
    bool wasAsked() const {
        return m_asked;
    }

protected:
    LC_FileDialogService::FileDialogResult askSaveFileDetails(const QString& currentFileName,
                                                              RS2::FormatType preferredType) override;

private:
    QString m_path;
    RS2::FormatType m_type = RS2::FormatUnknown;
    bool m_asked = false;
};

#endif
