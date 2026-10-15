/*
 * ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * Copyright (C) 2025 LibreCAD.org
 * Copyright (C) 2025 sand1024
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * ********************************************************************************
 */

#include "lc_releasechecker.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <qjsondocument.h>

#include "rs_debug.h"
#include "rs_dialogfactory.h"
#include "rs_dialogfactoryinterface.h"
#include "rs_settings.h"

namespace {
// A failed update check must never interrupt the user with a modal dialog.
// The check runs at startup and can be re-triggered from the menu, so log a
// warning to the console at most once per LibreCAD session, then stay silent.

// fixme - sand - well, this is actually disputable - nobody look at the console.
// but if the user set that noficiation is expected - it might be notified.
// probably instead of console it might be message in statusbar - yet
// actually I don't think modal is evil there.
// yet let it be silent - not too important thing.

void warnUpdateCheckFailedOnce(const QString &detail) {
    static bool warned = false;
    if (warned)
        return;
    warned = true;
    RS_Debug::Log(RS_Debug::D_WARNING)
        << "Checking for updates failed: " << detail;
}

// librecad-mcp: release tags of the LibreCAD-mcp fork are "v<LibreCAD version>-mcp.<N>",
// for example v2.2.2.6-mcp.1. N is the fork's build number (LC_MCP_RELEASE): it is global
// and only goes up, also when the LibreCAD version underneath changes. parseTagInfo() takes
// the "-mcp.N" part off a tag, parses the rest as before and keeps "mcp.N" in the label,
// where mcpReleaseOf() finds it again. A tag without the suffix has N = 0.
const QRegularExpression& mcpTagPattern() {
    static const QRegularExpression pattern(QStringLiteral(R"(^(.*)-mcp\.?(\d+)(?:[-_+].*)?$)"),
                                            QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

int mcpReleaseOf(const QString &label) {
    static const QRegularExpression pattern(QStringLiteral(R"((?:^|[-_])mcp\.?(\d+)$)"),
                                            QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(label);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}
} // namespace

LC_TagInfo::LC_TagInfo(int majorVer, int minorVer, int revisionNum, int bugfixVer, const QString &labelVer, const QString &tagNameVer):m_major(majorVer), m_minor(minorVer), m_revision(
                                                                                                                                          revisionNum), m_bugfix(bugfixVer), m_label(labelVer), m_tagName(tagNameVer) {
}

QString LC_TagInfo::getLabel() const {
    return m_label;
}

QString LC_TagInfo::getTagName() const {
    return m_tagName;
}

bool LC_TagInfo::isSameVersion(const LC_TagInfo& other) const {
    return m_major == other.m_major && m_minor == other.m_minor && m_revision == other.m_revision && m_bugfix == other.m_bugfix
        && mcpReleaseOf(m_label) == mcpReleaseOf(other.m_label); // librecad-mcp: and the same -mcp.N
}

bool LC_TagInfo::isBefore(const LC_TagInfo& other) const {
    // librecad-mcp: the fork's build number orders first, so a LibreCAD-mcp build is offered
    // every later -mcp.N release and never a tag without the suffix. Equal numbers (and tags
    // without the suffix, N = 0) compare by version as before.
    const int mcpRelease = mcpReleaseOf(m_label);
    const int otherMcpRelease = mcpReleaseOf(other.m_label);
    if (mcpRelease != otherMcpRelease) {
        return mcpRelease < otherMcpRelease;
    }
    if (m_major < other.m_major) {
        return true;
    }
    if (m_major == other.m_major) {
        if (m_minor < other.m_minor) {
            return true;
        }
        if (m_minor == other.m_minor) {
            if (m_revision < other.m_revision) {
                return true;
            }
            if (m_revision == other.m_revision) {
                if (m_bugfix < other.m_bugfix) {
                    return true;
                }
            }
        }
    }
    return false;
}

LC_ReleaseChecker::LC_ReleaseChecker(const QString& ownTagName, const bool ownPreRelease) :
    m_emitSignalIfNoNewVersion{false}, m_ownReleaseInfo(getOwnReleaseInfo(ownTagName, ownPreRelease)) {
    connect(&m_webCtrl, &QNetworkAccessManager::finished, this, &LC_ReleaseChecker::infoReceived);
}

LC_ReleaseInfo LC_ReleaseChecker::getOwnReleaseInfo(const QString& tagName, const bool preRelease) const {
    constexpr bool draft = false;
    QString ownTagName = tagName;
#ifdef LC_MCP_RELEASE
    // librecad-mcp: LC_VERSION never carries the fork's build number, so add it: version
    // 2.2.2.6-alpha with LC_MCP_RELEASE 1 compares as 2.2.2.6-alpha-mcp.1.
    if (!mcpTagPattern().match(ownTagName).hasMatch()) {
        ownTagName += QStringLiteral("-mcp.%1").arg(LC_MCP_RELEASE);
    }
#endif
    const LC_TagInfo ownTagInfo = parseTagInfo(ownTagName);
    auto result = LC_ReleaseInfo("", draft, preRelease, "", "");
    result.setTagInfo(ownTagInfo);
    return result;
}

#define TEST_JSON_PARSING_

void LC_ReleaseChecker::checkForNewVersion(const bool forceCheck) {
#ifndef TEST_JSON_PARSING
    m_emitSignalIfNoNewVersion = forceCheck;
    // librecad-mcp: a LibreCAD-mcp build looks for LibreCAD-mcp releases. Upstream asks
    // https://api.github.com/repos/LibreCAD/LibreCAD/releases here.
    const QUrl gitHubReleasesUrl("https://api.github.com/repos/ShiftSad/LibreCAD-mcp/releases");
    const QNetworkRequest request(gitHubReleasesUrl);
    m_webCtrl.get(request);
#else
    QFile file = QFile("c:/Local/LibreCADProjects/releases.json"); file.open(QIODevice::ReadOnly | QIODevice::Text); QByteArray
        responseContent = file.readAll(); file.close(); QString resultStr = QString(responseContent);
    //    LC_ERR << resultStr;
    processReleasesJSON(responseContent);
#endif
}

void LC_ReleaseChecker::infoReceived(QNetworkReply* reply) {
    if (reply->error() == QNetworkReply::NoError) {
        const QByteArray responseContent = reply->readAll();
        // auto resultStr = QString(responseContent);
        processReleasesJSON(responseContent);
    }
    else {
        warnUpdateCheckFailedOnce(reply->errorString());
    }
    reply->deleteLater();
}

LC_TagInfo LC_ReleaseChecker::parseTagInfo(const QString& tagName) const {
    // librecad-mcp: parse "v2.2.2.6-mcp.1" as "2.2.2.6" plus build number 1 (see mcpTagPattern)
    QString versionPart = tagName;
    int mcpRelease = 0;
    const QRegularExpressionMatch mcpMatch = mcpTagPattern().match(tagName);
    if (mcpMatch.hasMatch()) {
        versionPart = mcpMatch.captured(1);
        mcpRelease = mcpMatch.captured(2).toInt();
    }
    if (versionPart.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        versionPart.remove(0, 1);
    }
    const QStringList& split = versionPart.split(".");
    const qsizetype partsSize = split.size();
    int major = 0;
    int minor = 0;
    int revision = 0;
    int bugfix = 0;
    QString label = "";
    if (partsSize > 1) {
        major = split[0].toInt();
        minor = split[1].toInt();
        if (partsSize == 3) {
            const QString remaining = split[2];
            QStringList other = remaining.split("_");
            if (other.size() == 1) {
                other = remaining.split("-");
            }
            if (other.size() == 1) {
                bool ok = false;
                revision = remaining.toInt(&ok);
                if (!ok) {
                    label = remaining;
                }
            }
            else if (other.size() == 2) {
                revision = other[0].toInt();
                label = other[1];
            }
        }
        else if (partsSize == 4) {
            revision = split[2].toInt();
            const QString remaining = split[3];
            QStringList other = remaining.split("_");
            if (other.size() == 1) {
                other = remaining.split("-");
            }
            if (other.size() == 1) {
                bool ok = false;
                bugfix = remaining.toInt(&ok);
                if (!ok) {
                    label = remaining;
                }
            }
            else if (other.size() == 2) {
                bugfix = other[0].toInt();
                label = other[1];
            }
        }
    }
    if (mcpRelease > 0) { // librecad-mcp: keep the build number in the label, for mcpReleaseOf()
        label = label.isEmpty() ? QStringLiteral("mcp.%1").arg(mcpRelease)
                                : QStringLiteral("%1-mcp.%2").arg(label).arg(mcpRelease);
    }
    auto result = LC_TagInfo(major, minor, revision, bugfix, label, tagName);
    return result;
}

void LC_ReleaseChecker::processReleasesJSON(const QByteArray& responseContent) {
    QJsonParseError error;
    QJsonDocument jsonDocument = QJsonDocument::fromJson(responseContent, &error);
    if (jsonDocument.isEmpty()) {
        // fixme - sand - and this is acutually bad idea to ignore format error silently...
        warnUpdateCheckFailedOnce(QStringLiteral("unable to parse response from the server: %1")
                                  .arg(error.errorString()));
    }
    else {
        if (jsonDocument.isArray()) {
            QJsonArray array = jsonDocument.array();
            QVector<LC_ReleaseInfo> releases;
            QVector<LC_ReleaseInfo> preReleases;
            LC_TagInfo ownTagInfo = m_ownReleaseInfo.getTagInfo();
            LC_GROUP_GUARD("Startup");
            bool ignorePreReleases = LC_GET_BOOL("IgnorePreReleaseVersions", true);
            QString ignoredReleaseTagStr = LC_GET_STR("IgnoredRelease", "0.0.0.0");
            QString ignoredPreReleaseTag = LC_GET_STR("IgnoredPreRelease", "0.0.0.0");

            LC_TagInfo ignoredRelease = parseTagInfo(ignoredReleaseTagStr);
            LC_TagInfo ignoredPreRelease = parseTagInfo(ignoredPreReleaseTag);
            if (m_ownReleaseInfo.m_isPrerelease) {
                ignorePreReleases = false;
            }

            for (const QJsonValue& value : std::as_const(array)) {
                const QString& tagName = value["tag_name"].toString();
                const bool draft = value["draft"].toBool();
                const bool prerelease = value["prerelease"].toBool();
                const QString& publishedAt = value["published_at"].toString();
                const QString& body = value["body"].toString();
                const QString& htmlUrl = value["html_url"].toString();

                LC_TagInfo tagInfo = parseTagInfo(tagName);

                //                LC_ERR << tagName << " as  | " << tagInfo.major << "." << tagInfo.minor << "." << tagInfo.revision << "." << tagInfo.bugfix<< "-" << tagInfo.label;

                auto releaseInfo = LC_ReleaseInfo(publishedAt, draft, prerelease, htmlUrl, body);
                releaseInfo.setTagInfo(tagInfo);
                // first, filter everything by version numbers
                if (ownTagInfo.isBefore(tagInfo)) {
                    // also, if needed, skip pre-release builds
                    if (prerelease) {
                        if (ignorePreReleases) {
                            //                            LC_ERR << "Skipped as ignored pre-release";
                            continue;
                        }
                        if (tagInfo.isBefore(ignoredPreRelease)) {
                            //                            LC_ERR << "Skipped as before current";
                            continue;
                        }
                        preReleases << releaseInfo;
                    }
                    else {
                        if (tagInfo.isBefore(ignoredRelease)) {
                            //                            LC_ERR << "Skipped as before ignored";
                            continue;
                        }
                        releases << releaseInfo;
                    }
                }
                //              LC_ERR << publishedAt << " - as - " << releaseInfo.publishedDate.toString(Qt::ISODate);
            }

            qsizetype availableReleases = releases.size();
            qsizetype availablePreReleases = preReleases.size();

            // it might be that there are several releases with the same version but different suffix (i.e - latest).
            // for sure, in order to distinguish them it's more reliable to check and compare release dates.
            // however, that will require additional magic with embedding release date (similar to LC_Version), so
            // we can avoid such situations just by proper numbering of releases. In other words, each version should be
            // unique (say, different by last "revision" component of version number.

            if (availableReleases > 0) {
                sortReleasesInfo(releases);
                //                LC_ERR << availableReleases;
                LC_ReleaseInfo& info = releases.last();
                if (!info.getTagInfo().isSameVersion(ignoredRelease) && !info.getTagInfo().isSameVersion(ownTagInfo)) {
                    m_latestRelease = info;
                }
                else {
                    m_latestRelease.m_valid = false;
                    availableReleases = 0;
                }
            }
            //            LC_ERR << "=======";
            if (availablePreReleases > 0) {
                sortReleasesInfo(preReleases);
                //                LC_ERR << availablePreReleases;
                LC_ReleaseInfo& info = preReleases.last();
                if (!info.getTagInfo().isSameVersion(ignoredPreRelease) && !info.getTagInfo().isSameVersion(ownTagInfo)) {
                    m_latestPreRelease = info;
                }
                else {
                    m_latestRelease.m_valid = false;
                    availablePreReleases = 0;
                }
            }
            if (availableReleases > 0 || availablePreReleases > 0 || m_emitSignalIfNoNewVersion) {
                emit updatesAvailable();
                m_emitSignalIfNoNewVersion = false;
            }
        }
    }
}

void LC_ReleaseChecker::sortReleasesInfo(QVector<LC_ReleaseInfo>& list) const {
    std::sort(list.begin(), list.end(), [](const LC_ReleaseInfo& v0, const LC_ReleaseInfo& v1) {
        bool result = v0.m_tagInfo.isSameVersion(v1.m_tagInfo);
        if (result) {
            // if releases has the same version (mostly pre-releases) - just sort them by date and use the freshest one
            result = v1.m_publishedDate < v0.m_publishedDate;
        }
        else {
            result = v0.m_tagInfo.isBefore(v1.m_tagInfo);
        }

        return result;
    });
    //    for (auto ri: list){
    //        LC_ERR << ri.tagInfo.tagName;
    //    }
}

const LC_ReleaseInfo& LC_ReleaseChecker::getLatestRelease() const {
    return m_latestRelease;
}

const LC_ReleaseInfo& LC_ReleaseChecker::getLatestPreRelease() const {
    return m_latestPreRelease;
}
