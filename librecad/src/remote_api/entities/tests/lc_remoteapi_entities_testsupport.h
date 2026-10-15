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

// Scaffolding of the Remote API entities tests: a drawing in a headless view,
// JSON helpers, and calls of the route handlers with LC_ApiCall::setDoc().

#ifndef LC_REMOTEAPI_ENTITIES_TESTSUPPORT_H
#define LC_REMOTEAPI_ENTITIES_TESTSUPPORT_H

#include <cmath>
#include <functional>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMultiHash>
#include <QString>

#include "lc_actiontestsupport.h"
#include "lc_graphicviewport.h"
#include "lc_remoteapicontext.h"
#include "lc_remoteapientityindex.h"
#include "lc_remoteapientityops.h"
#include "lc_remoteapijson.h"
#include "lc_remoteapilayerops.h"
#include "lc_remoteapirouter.h"
#include "lc_remoteapitypes.h"
#include "rs_document.h"
#include "rs_entity.h"
#include "rs_graphic.h"
#include "rs_layer.h"

namespace lc::test::remoteapi {

/** A JSON object from text; the test fails on a syntax error. */
inline QJsonObject object(const char* text) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray(text), &error);
    INFO(text);
    REQUIRE(error.error == QJsonParseError::NoError);
    REQUIRE(document.isObject());
    return document.object();
}

inline QString compact(const QJsonObject& o) {
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

/** The LC_ApiError fn throws; the test fails when it throws none. */
inline LC_ApiError errorOf(const std::function<void()>& fn) {
    try {
        fn();
    }
    catch (const LC_ApiError& e) {
        return e;
    }
    FAIL("expected an LC_ApiError");
    return LC_ApiError(0, QString());
}

inline bool near(const double a, const double b, const double tolerance = 1e-6) {
    return std::abs(a - b) <= tolerance;
}

inline bool nearPoint(const QJsonValue& v, const double x, const double y, const double tolerance = 1e-6) {
    const QJsonArray a = v.toArray();
    return a.size() >= 2 && near(a.at(0).toDouble(), x, tolerance) && near(a.at(1).toDouble(), y, tolerance);
}

/** Whether two JSON values are equal, numbers within a tolerance and strings ignoring case. */
inline bool sameJson(const QJsonValue& a, const QJsonValue& b) {
    if (a.isDouble() && b.isDouble()) {
        return near(a.toDouble(), b.toDouble(), 1e-6);
    }
    if (a.isString() && b.isString()) {
        return a.toString().compare(b.toString(), Qt::CaseInsensitive) == 0;
    }
    if (a.isArray() && b.isArray()) {
        const QJsonArray x = a.toArray();
        const QJsonArray y = b.toArray();
        if (x.size() != y.size()) {
            return false;
        }
        for (int i = 0; i < x.size(); ++i) {
            if (!sameJson(x.at(i), y.at(i))) {
                return false;
            }
        }
        return true;
    }
    if (a.isObject() && b.isObject()) {
        const QJsonObject x = a.toObject();
        const QJsonObject y = b.toObject();
        if (x.keys() != y.keys()) {
            return false;
        }
        for (const QString& key : x.keys()) {
            if (!sameJson(x.value(key), y.value(key))) {
                return false;
            }
        }
        return true;
    }
    return a == b;
}

/** A new drawing in a 640x480 headless view. */
struct Drawing {
    const bool m_qtReady{lc::test::application() != nullptr};
    RS_Graphic graphic;
    lc::test::TestGraphicView view;

    Drawing() {
        graphic.initForNewDocument();
        graphic.onLoadingCompleted();
        view.setDocument(&graphic);
    }

    LC_GraphicViewport* viewport() const {
        return view.getViewPort();
    }

    LC_RemoteApiEntityOps ops() {
        return {&graphic, viewport()};
    }

    LC_RemoteApiLayerOps layers() {
        return {&graphic, viewport()};
    }

    LC_ResolvedDoc resolved() {
        LC_ResolvedDoc doc;
        doc.id = 1;
        doc.document = &graphic;
        doc.graphic = &graphic;
        doc.viewport = viewport();
        return doc;
    }

    /** Live top-level entities. */
    int liveCount() const {
        int n = 0;
        for (const RS_Entity* e : graphic) {
            n += LC_RemoteApiEntityIndex::isLive(e, &graphic) ? 1 : 0;
        }
        return n;
    }

    RS_Entity* entity(const unsigned long long id) {
        const LC_RemoteApiEntityIndex index(&graphic);
        return index.find(id);
    }

    /** POST /entities with the body; returns the response object. */
    QJsonObject create(const char* body) {
        QJsonArray warnings;
        QString audit;
        return ops().create(LC_JsonReader(object(body)), warnings, audit);
    }

    /** The id of the only entity a create body makes. */
    unsigned long long createOne(const char* body) {
        const QJsonArray ids = create(body).value(QStringLiteral("ids")).toArray();
        REQUIRE(ids.size() == 1);
        return static_cast<unsigned long long>(ids.at(0).toInteger());
    }

    RS_Layer* addLayer(const QString& name) {
        auto* layer = new RS_Layer(name);
        graphic.addLayer(layer);
        return graphic.findLayer(name);
    }
};

/** One handler call as the router makes it, with the document already resolved. */
struct HandlerCall {
    LC_ApiRequest request;
    LC_RemoteApiContext context{nullptr};
    std::optional<LC_ApiCall> call;

    HandlerCall(Drawing& drawing, const char* method, const QString& path, const QByteArray& body = {},
                const QMultiHash<QString, QString>& query = {}, const QHash<QString, QString>& params = {}) {
        request.method = method;
        request.path = path;
        request.body = body;
        request.query = query;
        call.emplace(request, context);
        call->params = params;
        call->setDoc(drawing.resolved());
    }

    /** The handler's response, or the envelope of the LC_ApiError it throws. */
    LC_ApiResponse run(const LC_ApiHandler& handler) {
        try {
            return handler(*call);
        }
        catch (const LC_ApiError& e) {
            return e.toResponse();
        }
    }
};

} // namespace lc::test::remoteapi

#endif
