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

#ifndef LC_REMOTEAPITYPES_H
#define LC_REMOTEAPITYPES_H

// Request, response and error types of the in-app Remote API (HTTP contract v1).
// Header-only. Pinned interface: the entities and documents components code
// against it, so change it only together with every user.

#include <exception>
#include <utility>

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMultiHash>
#include <QPair>
#include <QString>

/**
 * One parsed HTTP request, as handed to the router by the server thread.
 */
struct LC_ApiRequest {
    QByteArray method;                         // "GET","POST","PATCH","DELETE"
    QString path;                              // decoded, no query
    QMultiHash<QString, QString> query;        // decoded; repeated keys kept
    QHash<QByteArray, QByteArray> headers;     // lower-case names
    QByteArray body;
    // The path as received (still percent-encoded), no query. The router splits
    // it on '/' before decoding each segment, so an encoded "%2F" stays inside
    // its {param}; when empty (requests built in code) the router splits path.
    QString rawPath;
};

/**
 * A handler's answer. JSON by default; a raw body (e.g. PNG) when raw is set.
 */
struct LC_ApiResponse {
    int status = 200;
    QJsonObject json;                          // used when raw is empty
    QByteArray raw, contentType;               // e.g. PNG
    QList<QPair<QByteArray, QByteArray>> extraHeaders;   // e.g. X-LibreCAD-Window

    static LC_ApiResponse ok(QJsonObject o, int status = 200);
    static LC_ApiResponse bytes(QByteArray data, QByteArray type);
};

/**
 * Failure of a request. Handlers throw it; the router and the GUI runner turn it
 * into the error envelope {"error": token, "detail": ..., ...extra}.
 * Tokens and statuses are the catalog of the design (section 2.2): for example
 * LC_ApiError(404, "unknown_entity", "no such entity", {{"ids", ids}}).
 */
class LC_ApiError : public std::exception {
public:
    LC_ApiError(int status, QString token, QString detail = {}, QJsonObject extra = {});
    int status; QString token, detail; QJsonObject extra;      // extra: hint, param, allowed, path, ids, errors, ...
    LC_ApiResponse toResponse() const;                          // {"error":token,"detail":..., ...extra}
    const char* what() const noexcept override;

private:
    QByteArray m_what;                         // "token: detail", kept alive for what()
};

inline LC_ApiResponse LC_ApiResponse::ok(QJsonObject o, int status) {
    LC_ApiResponse response;
    response.status = status;
    response.json = std::move(o);
    return response;
}

inline LC_ApiResponse LC_ApiResponse::bytes(QByteArray data, QByteArray type) {
    LC_ApiResponse response;
    response.raw = std::move(data);
    response.contentType = std::move(type);
    return response;
}

inline LC_ApiError::LC_ApiError(int httpStatus, QString errorToken, QString errorDetail, QJsonObject extraFields)
    : status(httpStatus)
    , token(std::move(errorToken))
    , detail(std::move(errorDetail))
    , extra(std::move(extraFields))
    , m_what((detail.isEmpty() ? token : token + QStringLiteral(": ") + detail).toUtf8()) {
}

inline LC_ApiResponse LC_ApiError::toResponse() const {
    // "error" and "detail" are owned by the envelope; extra cannot override them.
    QJsonObject body = extra;
    body.insert(QStringLiteral("error"), token);
    if (detail.isEmpty()) {
        body.remove(QStringLiteral("detail"));
    } else {
        body.insert(QStringLiteral("detail"), detail);
    }
    return LC_ApiResponse::ok(std::move(body), status);
}

inline const char* LC_ApiError::what() const noexcept {
    return m_what.constData();
}

#endif
