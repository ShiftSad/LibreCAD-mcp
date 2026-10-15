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

#include "lc_remoteapipathpolicy.h"

#include <cstring>
#include <optional>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <sys/stat.h>
#endif

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

#ifdef Q_OS_WIN
/**
 * The name CreateFileW opens at any length: "\\?\C:\..." or "\\?\UNC\host\share\...".
 * Without the prefix a path longer than MAX_PATH (260) cannot be opened here,
 * while Qt's file classes add it themselves and reach the file.
 */
std::wstring longNativeName(const QString& absolutePath) {
    QString p = QDir::cleanPath(withSlashes(absolutePath));
    if (p.startsWith(QLatin1String("//?/")) || p.startsWith(QLatin1String("//./"))) {
        // already in the device namespace
    }
    else if (p.startsWith(QLatin1String("//"))) {
        p = QStringLiteral("//?/UNC/") + p.mid(2);
    }
    else {
        p = QStringLiteral("//?/") + p;
    }
    return QDir::toNativeSeparators(p).toStdWString();
}
#endif

/**
 * The final path of an existing file or folder, with every link resolved:
 * symbolic links and, on Windows, every other reparse point too (directory
 * junctions and volume mount points, which QFileInfo::canonicalFilePath()
 * leaves as they are), at any length. Empty when it cannot be resolved: the
 * caller then refuses the path, since the text alone could lead anywhere.
 */
QString resolvedExisting(const QString& existingPath) {
#ifdef Q_OS_WIN
    // FILE_FLAG_BACKUP_SEMANTICS opens folders as well; no access right is
    // needed to ask for the final name.
    const std::wstring native = longNativeName(QFileInfo(existingPath).absoluteFilePath());
    const HANDLE handle = CreateFileW(native.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length >= buffer.size()) {
        buffer.resize(length + 1);
        length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    CloseHandle(handle);
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    QString result = withSlashes(QString::fromWCharArray(buffer.data(), static_cast<qsizetype>(length)));
    // "//?/C:/x" -> "C:/x"; "//?/UNC/host/share" -> "//host/share"
    if (result.startsWith(QLatin1String("//?/UNC/"), Qt::CaseInsensitive)) {
        result = QLatin1String("//") + result.mid(8);
    }
    else if (result.startsWith(QLatin1String("//?/"))) {
        result = result.mid(4);
    }
    return QDir::cleanPath(result);
#else
    return QFileInfo(existingPath).canonicalFilePath();
#endif
}

/**
 * The canonical form of path: of the file when it exists, else of its nearest
 * existing ancestor with the rest appended. Symbolic links, directory
 * junctions (Windows) and ".." are resolved, so a path cannot leave an allowed
 * folder through them. Empty when what exists of it cannot be resolved.
 */
QString canonicalForm(const QString& normalizedPath) {
    const QFileInfo info(normalizedPath);
    if (info.exists()) {
        return resolvedExisting(normalizedPath);
    }
    QStringList rest{info.fileName()};
    QDir dir = info.absoluteDir();
    while (!dir.exists()) {
        rest.prepend(dir.dirName());
        if (!dir.cdUp()) {
            return normalizedPath; // nothing of it exists: no link to follow
        }
    }
    QString base = resolvedExisting(dir.absolutePath());
    if (base.isEmpty()) {
        return {};
    }
    if (!base.endsWith(QLatin1Char('/'))) {
        base += QLatin1Char('/');
    }
    return base + rest.join(QLatin1Char('/'));
}

/** Which file on which volume: the same for every name of one file. */
struct FileIdentity {
    quint64 volume = 0;
    quint64 high = 0;
    quint64 low = 0;
    bool operator==(const FileIdentity& other) const {
        return volume == other.volume && high == other.high && low == other.low;
    }
};

/**
 * The identity of an existing file: on Windows its volume serial number and
 * file id (GetFileInformationByHandleEx), on POSIX st_dev and st_ino. Every
 * name of the file has it: through a junction or symbolic link, an 8.3 alias,
 * or a hard link. nullopt when the file cannot be opened.
 */
std::optional<FileIdentity> identityOf(const QString& path) {
    if (!QFileInfo::exists(path)) {
        return std::nullopt;
    }
#ifdef Q_OS_WIN
    const std::wstring native = longNativeName(QFileInfo(path).absoluteFilePath());
    const HANDLE handle = CreateFileW(native.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    std::optional<FileIdentity> identity;
    FILE_ID_INFO idInfo{};
    BY_HANDLE_FILE_INFORMATION basic{};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &idInfo, sizeof(idInfo))) {
        FileIdentity id;
        id.volume = idInfo.VolumeSerialNumber;
        static_assert(sizeof(idInfo.FileId.Identifier) == 16, "a 128-bit file id");
        std::memcpy(&id.high, idInfo.FileId.Identifier, 8);
        std::memcpy(&id.low, idInfo.FileId.Identifier + 8, 8);
        identity = id;
    }
    else if (GetFileInformationByHandle(handle, &basic)) {
        FileIdentity id;
        id.volume = basic.dwVolumeSerialNumber;
        id.high = basic.nFileIndexHigh;
        id.low = basic.nFileIndexLow;
        identity = id;
    }
    CloseHandle(handle);
    return identity;
#else
    struct stat status{};
    if (::stat(QFile::encodeName(path).constData(), &status) != 0) {
        return std::nullopt;
    }
    FileIdentity id;
    id.volume = static_cast<quint64>(status.st_dev);
    id.low = static_cast<quint64>(status.st_ino);
    return id;
#endif
}

/** The first control character (U+0000..U+001F, U+007F) of text, or -1. */
qsizetype controlCharacter(const QString& text) {
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char16_t c = text.at(i).unicode();
        if (c < 0x20 || c == 0x7f) {
            return i;
        }
    }
    return -1;
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
    // Two existing names: the same file on disk, whatever leads to it (a
    // junction, a symbolic link, an 8.3 alias, a hard link).
    const std::optional<FileIdentity> identityA = identityOf(a);
    const std::optional<FileIdentity> identityB = identityOf(b);
    if (identityA.has_value() && identityB.has_value()) {
        return *identityA == *identityB;
    }
    // Otherwise the names, with every link on the way resolved.
    const auto normalize = [](const QString& path) {
        const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        const QString canonical = canonicalForm(clean);
        return canonical.isEmpty() ? clean : canonical;
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
    if (canonical.isEmpty()) {
        return false; // fail closed
    }
    for (const QString& root : std::as_const(roots)) {
        if (isNetworkPath(root) && !m_config.allowNetworkPaths) {
            continue;
        }
        const QFileInfo rootInfo(root);
        if (!rootInfo.isAbsolute()) {
            continue;
        }
        const QString rootPath = rootInfo.exists() ? resolvedExisting(rootInfo.absoluteFilePath())
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
    // A NUL ends the name the operating system sees, so the checks below would
    // judge another name than the file written; no other control character
    // belongs in a file name either.
    if (const qsizetype at = controlCharacter(raw); at >= 0) {
        QString shown = raw;
        for (QChar& c : shown) {
            if (c.unicode() < 0x20 || c.unicode() == 0x7f) {
                c = QChar(0xFFFD);
            }
        }
        fail(400, "bad_path", QStringLiteral("the path contains a control character (at %1)").arg(at), shown);
    }

    const bool network = isNetworkPath(raw);
#ifdef Q_OS_WIN
    const QString p = withSlashes(raw);
    // "\\?\C:\x", "\\.\C:\x" and "\??\C:\x" name the device namespace, not a
    // share: through them a local file would skip the rules below (Allow
    // network paths admits \\server\share only)
    if (p.startsWith(QLatin1String("//?/")) || p.startsWith(QLatin1String("//./")) || p.startsWith(QLatin1String("/??/"))) {
        fail(403, "path_not_allowed",
             QStringLiteral("device namespace paths (\\\\?\\, \\\\.\\) are not allowed: write C:/... or \\\\server\\share\\..."), path);
    }
#endif
    if (network && !m_config.allowNetworkPaths) {
        fail(403, "path_not_allowed",
             QStringLiteral("network paths are not allowed (Preferences > Remote API)"), path);
    }
#ifdef Q_OS_WIN
    if (!network) {
        // "C:/x" only: "C:x" is relative to the drive's current folder and "/x" to the current drive
        const bool driveAbsolute = p.size() >= 3 && p.at(0).isLetter() && p.at(1) == QLatin1Char(':')
            && p.at(2) == QLatin1Char('/');
        if (!driveAbsolute) {
            fail(400, "bad_path", QStringLiteral("the path must be absolute (for example C:/drawings/a.dxf)"), path);
        }
    }
    // "C:/a/host.txt:hidden.pdf" (or "//host/share/host.txt:hidden.pdf") names an
    // alternate data stream of host.txt: the extension and overwrite rules
    // would judge another file than the one written
    if (p.indexOf(QLatin1Char(':'), 2) >= 0) {
        fail(400, "bad_path",
             QStringLiteral("':' is only allowed after the drive letter (alternate data streams are not files)"), path);
    }
#else
    if (!network && !QDir::isAbsolutePath(raw)) {
        fail(400, "bad_path", QStringLiteral("the path must be absolute"), path);
    }
#endif

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

    // An existing file is judged by its own name too: on Windows the 8.3 alias
    // "KEEPME~1.DXF" names "keepme.dxfold", and a symbolic link "a.dxf" may lead
    // to "notes.txt". Writing through either would replace a file the rules
    // below never saw.
    QString resolved;
    QString resolvedName;
    if (info.exists()) {
        resolved = resolvedExisting(normalized);
        if (resolved.isEmpty() && !extensions.isEmpty()) {
            fail(400, "bad_path", QStringLiteral("the existing file cannot be resolved to its own name"), normalized);
        }
        const QString name = QFileInfo(resolved).fileName();
        if (!name.isEmpty() && name.compare(info.fileName(), PathCase) != 0) {
            resolvedName = name;
        }
    }
    const auto reject = [&](const int status, const char* token, const QString& detail, QJsonObject extra = {}) {
        if (!resolvedName.isEmpty()) {
            extra.insert(QStringLiteral("resolved"), resolved);
        }
        fail(status, token, detail, normalized, extra);
    };
    QStringList names{info.fileName()};
    if (!resolvedName.isEmpty()) {
        names << resolvedName;
    }

#ifdef Q_OS_WIN
    // "COM1.dxf", "NUL.svg", "CONIN$.dxf", "COM\u00b9.dxf": a device on Windows before 11,
    // not a file (the superscript digits are reserved like the plain ones)
    static const QStringList devices{QStringLiteral("CON"),    QStringLiteral("PRN"),     QStringLiteral("AUX"),
                                     QStringLiteral("NUL"),    QStringLiteral("CONIN$"),  QStringLiteral("CONOUT$"),
                                     QStringLiteral("COM1"),   QStringLiteral("COM2"),    QStringLiteral("COM3"),
                                     QStringLiteral("COM4"),   QStringLiteral("COM5"),    QStringLiteral("COM6"),
                                     QStringLiteral("COM7"),   QStringLiteral("COM8"),    QStringLiteral("COM9"),
                                     QStringLiteral("LPT1"),   QStringLiteral("LPT2"),    QStringLiteral("LPT3"),
                                     QStringLiteral("LPT4"),   QStringLiteral("LPT5"),    QStringLiteral("LPT6"),
                                     QStringLiteral("LPT7"),   QStringLiteral("LPT8"),    QStringLiteral("LPT9"),
                                     QStringLiteral("COM\u00b9"), QStringLiteral("COM\u00b2"), QStringLiteral("COM\u00b3"),
                                     QStringLiteral("LPT\u00b9"), QStringLiteral("LPT\u00b2"), QStringLiteral("LPT\u00b3")};
    for (const QString& name : std::as_const(names)) {
        // "COM1 .dxf" and "COM1..dxf" are COM1 as well
        if (devices.contains(name.section(QLatin1Char('.'), 0, 0).trimmed(), Qt::CaseInsensitive)) {
            reject(400, "bad_path", QStringLiteral("'%1' is a reserved Windows device name").arg(name));
        }
    }
#endif

    if (!extensions.isEmpty()) {
        for (const QString& name : std::as_const(names)) {
            const QString suffix = QFileInfo(name).suffix();
            bool known = false;
            for (const QString& extension : extensions) {
                known = known || extension.compare(QLatin1Char('.') + suffix, Qt::CaseInsensitive) == 0;
            }
            if (!known || suffix.isEmpty()) {
                const QString wanted = extensions.join(QStringLiteral(" or "));
                reject(422, "extension_mismatch",
                       name == info.fileName()
                           ? QStringLiteral("the file extension must be %1").arg(wanted)
                           : QStringLiteral("'%1' names the existing file '%2', whose extension is not %3")
                                 .arg(info.fileName(), name, wanted),
                       {{QStringLiteral("allowed"), toArray(extensions)}});
            }
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
