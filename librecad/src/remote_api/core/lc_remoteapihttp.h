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

#ifndef LC_REMOTEAPIHTTP_H
#define LC_REMOTEAPIHTTP_H

// The minimal HTTP/1.1 subset the Remote API speaks (design sections 1.6 and 2.1):
// one request per connection, Content-Length bodies only, Connection: close.

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include "lc_remoteapitypes.h"

/**
 * Incremental parser of one HTTP request.
 *
 * feed() takes the bytes as they arrive; the state turns Complete once the
 * request line, the headers and Content-Length bytes of body are in, or Error
 * as soon as the request breaks a rule. An error carries the status and the
 * envelope token of the design's catalog:
 * - 400 bad_request: malformed request line, header or Content-Length, or a
 *   connection that ends before the request is complete (finish());
 * - 411 length_required: any Transfer-Encoding (chunked bodies are refused);
 * - 413 body_too_large: Content-Length above the body limit;
 * - 431 headers_too_large: request line plus headers above the header limit.
 * Header names are lower-cased. The path and the query are percent-decoded;
 * in the query '+' is a space as well, and repeated keys are all kept.
 */
class LC_HttpRequestParser {
public:
    enum class State { Incomplete, Complete, Error };

    static constexpr qsizetype DefaultMaxHeaderBytes = 16 * 1024;
    static constexpr qsizetype DefaultMaxBodyBytes = 4 * 1024 * 1024;

    explicit LC_HttpRequestParser(qsizetype maxHeaderBytes = DefaultMaxHeaderBytes,
                                  qsizetype maxBodyBytes = DefaultMaxBodyBytes);

    State feed(QByteArrayView data);
    // The peer stopped sending: a request still incomplete becomes 400.
    State finish();
    State state() const { return m_state; }
    // True when nothing at all was received (a connection opened and closed).
    bool isEmpty() const { return m_buffer.isEmpty() && !m_headersDone; }
    // True once the request line and every header are in (the body may still be coming).
    bool headersComplete() const { return m_headersDone; }

    const LC_ApiError& error() const { return m_error; }
    const LC_ApiRequest& request() const { return m_request; }
    LC_ApiRequest takeRequest();

private:
    State fail(int status, const QString& token, const QString& detail, const QJsonObject& extra = {});
    State parseHead(QByteArrayView head);

    qsizetype m_maxHeaderBytes;
    qsizetype m_maxBodyBytes;
    State m_state = State::Incomplete;
    QByteArray m_buffer;
    bool m_headersDone = false;
    qsizetype m_contentLength = 0;
    LC_ApiRequest m_request;
    LC_ApiError m_error{400, QStringLiteral("bad_request")};
};

namespace LC_RemoteApiHttp {
    // The full response: status line, the common headers of design section
    // 2.1 (Content-Type, Content-Length, Cache-Control: no-store,
    // X-Content-Type-Options: nosniff, Connection: close), the response's
    // extra headers, then the body (compact JSON, or raw bytes when set).
    QByteArray serializeResponse(const LC_ApiResponse& response);
    QByteArray reasonPhrase(int status);
    // Percent-decoding as UTF-8; with plusIsSpace (query strings) '+' is a space.
    QString urlDecode(QByteArrayView encoded, bool plusIsSpace);
}

#endif
