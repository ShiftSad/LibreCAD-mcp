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

#ifndef LC_REMOTEAPIPATHPOLICY_H
#define LC_REMOTEAPIPATHPOLICY_H

// File path rules of the Remote API (design section 1.6, rule 5), applied to
// every path a request names: open, save (in place), save_as and export.

#include <QString>
#include <QStringList>

#include "lc_remoteapiconfig.h"

class LC_RemoteApiPathPolicy {
public:
    enum class Access {
        Read,          // the file must exist (open)
        Write,         // a new file, or one replaced with overwrite (save_as, export)
        WriteInPlace   // the document's own file (save): only the location rules apply
    };

    explicit LC_RemoteApiPathPolicy(const LC_RemoteApiConfig& config);

    /**
     * Checks path for access and returns it normalized (absolute, clean, '/'
     * separators). Throws LC_ApiError, in this order:
     * - 400 bad_path (+path): empty, with a control character (a NUL would cut
     *   the name the system opens), with a ':' after the drive (an alternate
     *   data stream, Windows), relative (also drive-relative "C:x" and
     *   rooted "\x" on Windows), or a directory;
     * - 403 path_not_allowed (+path): a UNC, \\?\ or device path while
     *   AllowNetworkPaths is off, or outside every AllowedRoots folder (after
     *   resolving symbolic links and, on Windows, junctions and mount points);
     * - 400 bad_path (+path): a reserved Windows device name ("COM1.dxf",
     *   "COM\u00b9.dxf", "nul.svg"), in the path or in the name of the existing
     *   file it reaches;
     * - 422 extension_mismatch (+path, +allowed, +resolved?): extensions is not
     *   empty and the file's extension (".dxf", compared case-insensitively) is
     *   not in it; for an existing file, the extension of its own name too (an
     *   8.3 alias or a symbolic link names another file, given in resolved);
     * - Read: 404 file_not_found (+path);
     * - Write: 400 bad_path when the parent folder does not exist, and
     *   409 file_exists (+path) when the file exists and overwrite is false.
     */
    QString check(const QString& path, Access access, const QStringList& extensions = {},
                  bool overwrite = false) const;

    /** Whether path names a network share or device: \\host, //host, \\?\, \\.\ */
    static bool isNetworkPath(const QString& path);
    /**
     * Whether two paths name the same file: when both exist, the same file on
     * disk (volume and file id: a junction, a symbolic link, an 8.3 alias or a
     * hard link leads to it as well); otherwise the same name once every link
     * on the way is resolved (case-insensitive on Windows).
     */
    static bool sameFile(const QString& a, const QString& b);

private:
    bool isUnderAllowedRoot(const QString& normalizedPath) const;

    LC_RemoteApiConfig m_config;
};

#endif
