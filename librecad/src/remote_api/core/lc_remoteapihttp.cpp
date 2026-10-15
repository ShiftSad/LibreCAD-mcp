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

#include "lc_remoteapihttp.h"

#include <QJsonDocument>
#include <QList>

namespace {

bool isTokenChar(const char c) {
    // RFC 9110 tchar
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
        case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

bool isMethodName(const QByteArrayView method) {
    if (method.isEmpty() || method.size() > 16) {
        return false;
    }
    for (const char c : method) {
        if (c < 'A' || c > 'Z') {
            return false;
        }
    }
    return true;
}

bool hasControlChars(const QByteArrayView text) {
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if ((u < 0x20 && c != '\t') || u == 0x7f) {
            return true;
        }
    }
    return false;
}

// Removes CR and LF so that a header built from data cannot split the response.
QByteArray headerSafe(const QByteArray& text) {
    QByteArray result = text;
    result.replace('\r', ' ');
    result.replace('\n', ' ');
    return result;
}

} // namespace

LC_HttpRequestParser::LC_HttpRequestParser(const qsizetype maxHeaderBytes, const qsizetype maxBodyBytes)
    : m_maxHeaderBytes(maxHeaderBytes)
    , m_maxBodyBytes(maxBodyBytes) {
}

LC_HttpRequestParser::State LC_HttpRequestParser::fail(const int status, const QString& token,
                                                       const QString& detail, const QJsonObject& extra) {
    m_error = LC_ApiError(status, token, detail, extra);
    m_state = State::Error;
    m_buffer.clear();
    return m_state;
}

LC_HttpRequestParser::State LC_HttpRequestParser::feed(const QByteArrayView data) {
    if (m_state != State::Incomplete) {
        return m_state; // a complete request ignores what follows (no pipelining)
    }
    m_buffer.append(data);

    if (!m_headersDone) {
        const qsizetype end = m_buffer.indexOf("\r\n\r\n");
        if (end < 0) {
            if (m_buffer.size() > m_maxHeaderBytes) {
                return fail(431, QStringLiteral("headers_too_large"),
                            QStringLiteral("request line and headers exceed %1 bytes").arg(m_maxHeaderBytes),
                            {{QStringLiteral("limit"), static_cast<double>(m_maxHeaderBytes)}});
            }
            return m_state;
        }
        const qsizetype headSize = end + 4;
        if (headSize > m_maxHeaderBytes) {
            return fail(431, QStringLiteral("headers_too_large"),
                        QStringLiteral("request line and headers exceed %1 bytes").arg(m_maxHeaderBytes),
                        {{QStringLiteral("limit"), static_cast<double>(m_maxHeaderBytes)}});
        }
        const QByteArray head = m_buffer.left(end + 2); // every line keeps its CRLF
        m_buffer.remove(0, headSize);
        if (parseHead(head) == State::Error) {
            return m_state;
        }
        m_headersDone = true;
    }

    if (m_buffer.size() >= m_contentLength) {
        m_request.body = m_buffer.left(m_contentLength);
        m_buffer.clear();
        m_state = State::Complete;
    }
    return m_state;
}

LC_HttpRequestParser::State LC_HttpRequestParser::finish() {
    if (m_state != State::Incomplete) {
        return m_state;
    }
    if (!m_headersDone) {
        return fail(400, QStringLiteral("bad_request"), QStringLiteral("the connection ended before the headers were complete"));
    }
    return fail(400, QStringLiteral("bad_request"),
                QStringLiteral("the body is shorter than Content-Length (%1 of %2 bytes)")
                    .arg(m_buffer.size()).arg(m_contentLength));
}

LC_ApiRequest LC_HttpRequestParser::takeRequest() {
    return std::move(m_request);
}

LC_HttpRequestParser::State LC_HttpRequestParser::parseHead(const QByteArrayView head) {
    // head is the request line and the header lines, each ending with CRLF
    QList<QByteArray> lines = head.toByteArray().split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast();
    }
    if (lines.isEmpty()) {
        return fail(400, QStringLiteral("bad_request"), QStringLiteral("empty request"));
    }

    // Request line: METHOD SP request-target SP HTTP/1.x
    QByteArray requestLine = lines.first();
    if (!requestLine.endsWith('\r')) {
        return fail(400, QStringLiteral("bad_request"), QStringLiteral("lines must end with CRLF"));
    }
    requestLine.chop(1);
    const QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() != 3 || !isMethodName(parts[0]) || !parts[1].startsWith('/') ||
        (parts[2] != "HTTP/1.1" && parts[2] != "HTTP/1.0") || hasControlChars(parts[1])) {
        return fail(400, QStringLiteral("bad_request"), QStringLiteral("malformed request line"));
    }
    m_request.method = parts[0];
    m_http10 = parts[2] == "HTTP/1.0";

    const QByteArray& target = parts[1];
    const qsizetype queryStart = target.indexOf('?');
    const QByteArray rawPath = queryStart < 0 ? target : target.left(queryStart);
    const QByteArray rawQuery = queryStart < 0 ? QByteArray() : target.mid(queryStart + 1);
    m_request.rawPath = QString::fromLatin1(rawPath);
    m_request.path = LC_RemoteApiHttp::urlDecode(rawPath, false);

    // QMultiHash::values() lists the most recently inserted value first: insert
    // the pairs back to front so that values() returns them in request order.
    const QList<QByteArray> pairs = rawQuery.split('&');
    for (auto it = pairs.crbegin(); it != pairs.crend(); ++it) {
        if (it->isEmpty()) {
            continue;
        }
        const qsizetype eq = it->indexOf('=');
        const QByteArray key = eq < 0 ? *it : it->left(eq);
        const QByteArray value = eq < 0 ? QByteArray() : it->mid(eq + 1);
        m_request.query.insert(LC_RemoteApiHttp::urlDecode(key, true), LC_RemoteApiHttp::urlDecode(value, true));
    }

    bool hasContentLength = false;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        QByteArray line = lines.at(i);
        if (!line.endsWith('\r')) {
            return fail(400, QStringLiteral("bad_request"), QStringLiteral("lines must end with CRLF"));
        }
        line.chop(1);
        if (line.isEmpty()) {
            return fail(400, QStringLiteral("bad_request"), QStringLiteral("malformed header"));
        }
        if (line.startsWith(' ') || line.startsWith('\t')) {
            return fail(400, QStringLiteral("bad_request"), QStringLiteral("folded headers are not supported"));
        }
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) {
            return fail(400, QStringLiteral("bad_request"), QStringLiteral("malformed header"));
        }
        const QByteArray name = line.left(colon).toLower();
        for (const char c : name) {
            if (!isTokenChar(c)) {
                return fail(400, QStringLiteral("bad_request"), QStringLiteral("malformed header name"));
            }
        }
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (hasControlChars(value)) {
            return fail(400, QStringLiteral("bad_request"), QStringLiteral("control characters in header %1")
                                                                .arg(QString::fromLatin1(name)));
        }

        if (name == "transfer-encoding") {
            return fail(411, QStringLiteral("length_required"),
                        QStringLiteral("Transfer-Encoding is not supported; send the body with Content-Length"));
        }
        if (name == "content-length") {
            bool ok = !value.isEmpty() && value.size() <= 18;
            for (const char c : value) {
                ok = ok && c >= '0' && c <= '9';
            }
            const qint64 length = ok ? value.toLongLong(&ok) : -1;
            if (!ok || (hasContentLength && length != m_contentLength)) {
                return fail(400, QStringLiteral("bad_request"), QStringLiteral("invalid Content-Length"));
            }
            if (length > m_maxBodyBytes) {
                return fail(413, QStringLiteral("body_too_large"),
                            QStringLiteral("the body exceeds %1 bytes").arg(m_maxBodyBytes),
                            {{QStringLiteral("limit"), static_cast<double>(m_maxBodyBytes)}});
            }
            hasContentLength = true;
            m_contentLength = static_cast<qsizetype>(length);
        }
        if (name == "host" && m_request.headers.contains(name)) {
            return fail(400, QStringLiteral("bad_request"), QStringLiteral("more than one Host header"));
        }

        auto existing = m_request.headers.find(name);
        if (existing == m_request.headers.end()) {
            m_request.headers.insert(name, value);
        }
        else if (name != "content-length") {
            existing.value() += ", " + value;
        }
    }
    return m_state;
}

namespace LC_RemoteApiHttp {

QByteArray reasonPhrase(const int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 417: return "Expectation Failed";
        case 421: return "Misdirected Request";
        case 422: return "Unprocessable Content";
        case 431: return "Request Header Fields Too Large";
        case 499: return "Client Closed Request";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Unknown";
    }
}

QByteArray serializeResponse(const LC_ApiResponse& response) {
    QByteArray body;
    QByteArray contentType;
    if (!response.raw.isEmpty()) {
        body = response.raw;
        contentType = response.contentType.isEmpty() ? QByteArray("application/octet-stream") : response.contentType;
    }
    else {
        body = QJsonDocument(response.json).toJson(QJsonDocument::Compact);
        contentType = "application/json; charset=utf-8";
    }

    const int status = (response.status >= 100 && response.status <= 999) ? response.status : 500;
    QByteArray out;
    out.reserve(body.size() + 256);
    out += "HTTP/1.1 " + QByteArray::number(status) + ' ' + reasonPhrase(status) + "\r\n";
    out += "Content-Type: " + headerSafe(contentType) + "\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\n";
    out += "X-Content-Type-Options: nosniff\r\n";
    out += "Connection: close\r\n";
    for (const auto& [name, value] : response.extraHeaders) {
        const QByteArray safeName = headerSafe(name).trimmed();
        if (safeName.isEmpty() || safeName.contains(':')) {
            continue;
        }
        out += safeName + ": " + headerSafe(value) + "\r\n";
    }
    out += "\r\n";
    out += body;
    return out;
}

QString urlDecode(const QByteArrayView encoded, const bool plusIsSpace) {
    QByteArray bytes = encoded.toByteArray();
    if (plusIsSpace) {
        bytes.replace('+', ' ');
    }
    return QString::fromUtf8(QByteArray::fromPercentEncoding(bytes));
}

} // namespace LC_RemoteApiHttp
