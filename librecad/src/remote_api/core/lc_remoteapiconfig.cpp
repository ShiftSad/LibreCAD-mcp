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

#include "lc_remoteapiconfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>

#include "rs_settings.h"

namespace {

constexpr auto KeyEnabled = "Enabled";
constexpr auto KeyPort = "Port";
constexpr auto KeyToken = "Token";
constexpr auto KeyNotify = "Notify";
constexpr auto KeyAllowCommands = "AllowCommands";
constexpr auto KeyAllowedRoots = "AllowedRoots";
constexpr auto KeyAllowNetworkPaths = "AllowNetworkPaths";

constexpr auto DiscoveryFileName = "remote-api.json";

// RS_Settings stores booleans as 1/0 and reads them back as integers, so a
// hand-written "true" would read as false. Accept both spellings here.
bool readFlag(const char* key, const bool defaultValue) {
    const QString text = LC_GET_ONE_STR(LC_RemoteApiConfig::SettingsGroup, key,
                                        defaultValue ? QStringLiteral("1") : QStringLiteral("0"))
                             .trimmed().toLower();
    if (text == QLatin1String("1") || text == QLatin1String("true") || text == QLatin1String("yes") ||
        text == QLatin1String("on")) {
        return true;
    }
    if (text == QLatin1String("0") || text == QLatin1String("false") || text == QLatin1String("no") ||
        text == QLatin1String("off")) {
        return false;
    }
    return defaultValue;
}

} // namespace

bool LC_RemoteApiConfig::isValidPort(const int value) {
    return value >= MinPort && value <= MaxPort;
}

LC_RemoteApiConfig LC_RemoteApiConfig::load() {
    LC_RemoteApiConfig config;
    if (RS_SETTINGS == nullptr) {
        return config;
    }
    config.enabled = readFlag(KeyEnabled, false);
    bool portOk = false;
    const int storedPort = LC_GET_ONE_STR(SettingsGroup, KeyPort, QString::number(DefaultPort)).trimmed().toInt(&portOk);
    config.port = portOk && isValidPort(storedPort) ? storedPort : DefaultPort;
    config.token = LC_GET_ONE_STR(SettingsGroup, KeyToken, QString()).trimmed();
    config.notify = readFlag(KeyNotify, true);
    config.allowCommands = readFlag(KeyAllowCommands, true);
    const QString roots = LC_GET_ONE_STR(SettingsGroup, KeyAllowedRoots, QString());
    for (const QString& root : roots.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QString trimmed = root.trimmed();
        if (!trimmed.isEmpty()) {
            config.allowedRoots << trimmed;
        }
    }
    config.allowNetworkPaths = readFlag(KeyAllowNetworkPaths, false);
    return config;
}

void LC_RemoteApiConfig::save() const {
    if (RS_SETTINGS == nullptr) {
        return;
    }
    LC_SET_ONE(SettingsGroup, KeyEnabled, enabled);
    if (isValidPort(port)) {
        LC_SET_ONE(SettingsGroup, KeyPort, port);
    }
    LC_SET_ONE(SettingsGroup, KeyToken, token);
    LC_SET_ONE(SettingsGroup, KeyNotify, notify);
    LC_SET_ONE(SettingsGroup, KeyAllowCommands, allowCommands);
    LC_SET_ONE(SettingsGroup, KeyAllowedRoots, allowedRoots.join(QLatin1Char(';')));
    LC_SET_ONE(SettingsGroup, KeyAllowNetworkPaths, allowNetworkPaths);
}

QString LC_RemoteApiConfig::generateToken() {
    // QRandomGenerator::system() is the operating system's CSPRNG.
    quint32 words[4] = {};
    QRandomGenerator::system()->fillRange(words);
    QString token;
    token.reserve(32);
    for (const quint32 word : words) {
        token += QStringLiteral("%1").arg(word, 8, 16, QLatin1Char('0'));
    }
    token = token.toLower();
    if (token.size() != 32) {
        return {};
    }
    // All-zero output means the generator did not run: fail closed.
    if (token.count(QLatin1Char('0')) == token.size()) {
        return {};
    }
    return token;
}

bool LC_RemoteApiConfig::tokenEquals(const QByteArray& expected, const QByteArray& given) {
    if (expected.isEmpty() || expected.size() != given.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (qsizetype i = 0; i < expected.size(); ++i) {
        diff |= static_cast<unsigned char>(expected.at(i) ^ given.at(i));
    }
    return diff == 0;
}

QString LC_RemoteApiConfig::settingsSandboxDir() {
    return qEnvironmentVariable("LIBRECAD_SETTINGS_DIR").trimmed();
}

QString LC_RemoteApiConfig::discoveryFilePath() {
    const QString sandbox = settingsSandboxDir();
    if (!sandbox.isEmpty()) {
        return QDir(sandbox).absoluteFilePath(QLatin1String(DiscoveryFileName));
    }
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dataDir.isEmpty()) {
        return {};
    }
    return QDir(dataDir).absoluteFilePath(QLatin1String(DiscoveryFileName));
}

bool LC_RemoteApiConfig::writeDiscoveryFile(const QString& path, const QJsonObject& content, QString* error) {
    if (path.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("no location for the discovery file");
        }
        return false;
    }
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot create %1").arg(info.absolutePath());
        }
        return false;
    }
    constexpr QFileDevice::Permissions ownerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    // Restrict the temporary file before the token goes into it.
    file.setPermissions(ownerOnly);
    file.write(QJsonDocument(content).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    QFile::setPermissions(path, ownerOnly);
    return true;
}

QJsonObject LC_RemoteApiConfig::readDiscoveryFile(const QString& path) {
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.read(64 * 1024));
    return document.isObject() ? document.object() : QJsonObject();
}

bool LC_RemoteApiConfig::removeDiscoveryFile(const QString& path, const qint64 pid) {
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return true;
    }
    const QJsonObject content = readDiscoveryFile(path);
    if (static_cast<qint64>(content.value(QStringLiteral("pid")).toDouble(-1)) != pid) {
        return false; // another instance's file, or not ours to judge
    }
    return QFile::remove(path);
}
