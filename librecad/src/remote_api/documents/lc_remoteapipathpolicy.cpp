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

#include "lc_remoteapipathpolicy.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

#include "lc_remoteapitypes.h"

namespace {

#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity PathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity PathCase = Qt::CaseSensitive;
#endif

QString withSlashes(const QString& path) {
    QString result = path;
    result.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return result;
}

[[noreturn]] void fail(const int status, const char* token, const QString& detail, const QString& path,
                       QJsonObject extra = {}) {
    extra.insert(QStringLiteral("path"), path);
    throw LC_ApiError(status, QString::fromLatin1(token), detail, extra);
}

/**
 * The canonical form of path: of the file when it exists, else of its nearest
 * existing ancestor with the rest appended. Symbolic links and ".." are
 * resolved, so a path cannot leave an allowed folder through them.
 */
QString canonicalForm(const QString& normalizedPath) {
    const QFileInfo info(normalizedPath);
    if (info.exists()) {
        return info.canonicalFilePath();
    }
    QStringList rest{info.fileName()};
    QDir dir = info.absoluteDir();
    while (!dir.exists()) {
        rest.prepend(dir.dirName());
        if (!dir.cdUp()) {
            return normalizedPath;
        }
    }
    QString base = QFileInfo(dir.absolutePath()).canonicalFilePath();
    if (base.isEmpty()) {
        return normalizedPath;
    }
    if (!base.endsWith(QLatin1Char('/'))) {
        base += QLatin1Char('/');
    }
    return base + rest.join(QLatin1Char('/'));
}

bool isUnder(const QString& path, QString root) {
    while (root.size() > 1 && root.endsWith(QLatin1Char('/'))) {
        root.chop(1);
    }
    if (path.compare(root, PathCase) == 0) {
        return true;
    }
    return path.startsWith(root + QLatin1Char('/'), PathCase);
}

QJsonArray toArray(const QStringList& list) {
    QJsonArray array;
    for (const QString& item : list) {
        array.append(item);
    }
    return array;
}

} // namespace

LC_RemoteApiPathPolicy::LC_RemoteApiPathPolicy(const LC_RemoteApiConfig& config)
    : m_config(config) {
}

bool LC_RemoteApiPathPolicy::isNetworkPath(const QString& path) {
    const QString p = withSlashes(path.trimmed());
    // \\host\share, //host/share, \\?\..., \\.\device
    return p.startsWith(QLatin1String("//"));
}

bool LC_RemoteApiPathPolicy::sameFile(const QString& a, const QString& b) {
    if (a.isEmpty() || b.isEmpty()) {
        return false;
    }
    const auto normalize = [](const QString& path) {
        const QFileInfo info(path);
        const QString canonical = info.exists() ? info.canonicalFilePath() : QString();
        return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
    };
    return normalize(a).compare(normalize(b), PathCase) == 0;
}

bool LC_RemoteApiPathPolicy::isUnderAllowedRoot(const QString& normalizedPath) const {
    QStringList roots;
    for (const QString& root : m_config.allowedRoots) {
        if (!root.trimmed().isEmpty()) {
            roots.append(root.trimmed());
        }
    }
    if (roots.isEmpty()) {
        return true;   // no restriction
    }
    const QString canonical = canonicalForm(normalizedPath);
    for (const QString& root : std::as_const(roots)) {
        if (isNetworkPath(root) && !m_config.allowNetworkPaths) {
            continue;
        }
        const QFileInfo rootInfo(root);
        if (!rootInfo.isAbsolute()) {
            continue;
        }
        const QString rootPath = rootInfo.exists() ? rootInfo.canonicalFilePath()
                                                   : QDir::cleanPath(rootInfo.absoluteFilePath());
        if (!rootPath.isEmpty() && isUnder(canonical, rootPath)) {
            return true;
        }
    }
    return false;
}

QString LC_RemoteApiPathPolicy::check(const QString& path, const Access access, const QStringList& extensions,
                                      const bool overwrite) const {
    const QString raw = path.trimmed();
    if (raw.isEmpty()) {
        fail(400, "bad_path", QStringLiteral("the path is empty"), path);
    }

    const bool network = isNetworkPath(raw);
    if (network && !m_config.allowNetworkPaths) {
        fail(403, "path_not_allowed",
             QStringLiteral("network and device paths are not allowed (Preferences > Remote API)"), path);
    }
    if (!network) {
#ifdef Q_OS_WIN
        // "C:/x" only: "C:x" is relative to the drive's current folder and "/x" to the current drive
        const QString p = withSlashes(raw);
        const bool driveAbsolute = p.size() >= 3 && p.at(0).isLetter() && p.at(1) == QLatin1Char(':')
            && p.at(2) == QLatin1Char('/');
        if (!driveAbsolute) {
            fail(400, "bad_path", QStringLiteral("the path must be absolute (for example C:/drawings/a.dxf)"), path);
        }
#else
        if (!QDir::isAbsolutePath(raw)) {
            fail(400, "bad_path", QStringLiteral("the path must be absolute"), path);
        }
#endif
    }

    const QString normalized = QDir::cleanPath(QFileInfo(raw).absoluteFilePath());
    // the roots first: nothing is said about files outside them
    if (!isUnderAllowedRoot(normalized)) {
        fail(403, "path_not_allowed",
             QStringLiteral("the path is outside the folders the Remote API may use (Preferences > Remote API)"),
             normalized);
    }
    const QFileInfo info(normalized);
    if (info.isDir()) {
        fail(400, "bad_path", QStringLiteral("the path names a folder, not a file"), normalized);
    }

    if (!extensions.isEmpty()) {
        const QString suffix = QLatin1Char('.') + info.suffix().toLower();
        bool known = false;
        for (const QString& extension : extensions) {
            known = known || extension.compare(suffix, Qt::CaseInsensitive) == 0;
        }
        if (!known || info.suffix().isEmpty()) {
            fail(422, "extension_mismatch",
                 QStringLiteral("the file extension must be %1").arg(extensions.join(QStringLiteral(" or "))),
                 normalized, {{QStringLiteral("allowed"), toArray(extensions)}});
        }
    }

    switch (access) {
        case Access::Read:
            if (!info.exists()) {
                fail(404, "file_not_found", QStringLiteral("no such file"), normalized);
            }
            break;
        case Access::Write:
            if (!info.absoluteDir().exists()) {
                fail(400, "bad_path", QStringLiteral("the folder does not exist"), normalized);
            }
            if (info.exists() && !overwrite) {
                fail(409, "file_exists", QStringLiteral("the file exists; pass \"overwrite\": true to replace it"),
                     normalized);
            }
            break;
        case Access::WriteInPlace:
            break;
    }
    return normalized;
}
