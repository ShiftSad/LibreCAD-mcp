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

#include "lc_remoteapidocumentsstorage.h"

#include <QFileInfo>

void LC_RemoteApiDocumentsStorage::setTarget(const QString& path, const RS2::FormatType type) {
    m_path = path;
    m_type = type;
}

LC_FileDialogService::FileDialogResult LC_RemoteApiDocumentsStorage::askSaveFileDetails(
    [[maybe_unused]] const QString& currentFileName, [[maybe_unused]] const RS2::FormatType preferredType) {
    m_asked = true;
    LC_FileDialogService::FileDialogResult result;
    if (m_path.isEmpty() || m_type == RS2::FormatUnknown) {
        return result;   // an empty file path is a cancel
    }
    const QFileInfo info(m_path);
    result.dirPath = info.absolutePath();
    result.filePath = m_path;
    result.fileName = info.completeBaseName();
    result.fileExtension = QLatin1Char('.') + info.suffix();
    result.fileType = m_type;
    return result;
}
