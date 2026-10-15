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

#include "lc_remoteapientityindex.h"

#include <algorithm>
#include <cmath>

#include <QJsonArray>

#include "lc_containertraverser.h"
#include "lc_remoteapientityjson.h"
#include "lc_remoteapitypes.h"
#include "rs_document.h"
#include "rs_entity.h"
#include "rs_entitycontainer.h"
#include "rs_graphic.h"
#include "rs_information.h"
#include "rs_layer.h"
#include "rs_solid.h"

namespace {

namespace json = LC_RemoteApiEntityJson;

QJsonArray idsArray(const QList<unsigned long long>& ids) {
    QJsonArray out;
    for (const unsigned long long id : ids) {
        out.append(static_cast<qint64>(id));
    }
    return out;
}

QString checkTypeName(const QString& name, const QString& param) {
    const QString trimmed = name.trimmed().toLower();
    if (!json::allTypeNames().contains(trimmed)) {
        json::throwBadParam(param, QStringLiteral("unknown entity type '%1'").arg(name),
                            QJsonArray::fromStringList(json::allTypeNames()));
    }
    return trimmed;
}

std::pair<RS_Vector, RS_Vector> boxFromNumbers(const QList<double>& numbers, const QString& param) {
    if (numbers.size() != 4) {
        json::throwBadParam(param, QStringLiteral("'%1' must be four numbers x1,y1,x2,y2").arg(param));
    }
    for (const double n : numbers) {
        if (!std::isfinite(n)) {
            json::throwBadParam(param, QStringLiteral("'%1' must be four finite numbers").arg(param));
        }
    }
    return {RS_Vector(numbers.at(0), numbers.at(1)), RS_Vector(numbers.at(2), numbers.at(3))};
}

bool parseBoxMode(const QString& mode, const QString& param) {
    if (mode == QLatin1String("crossing")) {
        return true;
    }
    if (mode == QLatin1String("window")) {
        return false;
    }
    json::throwBadParam(param, QStringLiteral("'%1' must be crossing or window").arg(param),
                        QJsonArray{QStringLiteral("crossing"), QStringLiteral("window")});
}

bool boxesOverlap(const RS_Vector& min1, const RS_Vector& max1, const RS_Vector& min2, const RS_Vector& max2) {
    return !(max1.x < min2.x - RS_TOLERANCE || min1.x > max2.x + RS_TOLERANCE || max1.y < min2.y - RS_TOLERANCE ||
             min1.y > max2.y + RS_TOLERANCE);
}

bool touchesEdges(const RS_Entity* e, const RS_EntityContainer& edges, const RS_Vector& corner1, const RS_Vector& corner2) {
    if (e->rtti() == RS2::EntitySolid) {
        return static_cast<const RS_Solid*>(e)->isInCrossWindow(corner1, corner2);
    }
    for (const RS_Entity* edge : edges) {
        if (RS_Information::getIntersection(e, edge, true).hasValid()) {
            return true;
        }
    }
    return false;
}

} // namespace

// --- filter ------------------------------------------------------------------

LC_RemoteApiEntityFilter LC_RemoteApiEntityFilter::fromQuery(const QMultiHash<QString, QString>& query, RS_Graphic* graphic) {
    LC_RemoteApiEntityFilter filter;
    // QMultiHash::values() gives the most recent first; keep the request order
    QStringList typeValues = query.values(QStringLiteral("type"));
    std::reverse(typeValues.begin(), typeValues.end());
    for (const QString& value : std::as_const(typeValues)) {
        for (const QString& part : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString name = checkTypeName(part, QStringLiteral("type"));
            if (!filter.types.contains(name)) {
                filter.types.append(name);
            }
        }
    }
    QStringList layerValues = query.values(QStringLiteral("layer"));
    std::reverse(layerValues.begin(), layerValues.end());
    for (const QString& name : std::as_const(layerValues)) {
        RS_Layer* layer = LC_RemoteApiEntityIndex::requireLayer(graphic, name);
        if (!filter.layers.contains(layer)) {
            filter.layers.append(layer);
        }
    }
    if (const auto bbox = LC_RemoteApiQuery::single(query, QStringLiteral("bbox"))) {
        QList<double> numbers;
        for (const QString& part : bbox->split(QLatin1Char(','))) {
            bool ok = false;
            const double n = part.trimmed().toDouble(&ok);
            numbers.append(ok ? n : std::nan(""));
        }
        filter.box = boxFromNumbers(numbers, QStringLiteral("bbox"));
    }
    if (const auto mode = LC_RemoteApiQuery::single(query, QStringLiteral("bbox_mode"))) {
        filter.crossing = parseBoxMode(*mode, QStringLiteral("bbox_mode"));
    }
    filter.selectedOnly = LC_RemoteApiQuery::flag(query, QStringLiteral("selected"), false);
    filter.includeHidden = LC_RemoteApiQuery::flag(query, QStringLiteral("include_hidden"), false);
    return filter;
}

LC_RemoteApiEntityFilter LC_RemoteApiEntityFilter::fromBody(const QJsonObject& body, RS_Graphic* graphic) {
    LC_RemoteApiEntityFilter filter;
    const auto stringList = [&body](const QString& key) {
        QStringList out;
        const QJsonValue v = body.value(key);
        if (v.isUndefined() || v.isNull()) {
            return out;
        }
        if (v.isString()) {
            out << v.toString();
            return out;
        }
        if (!v.isArray()) {
            json::throwBadParam(key, QStringLiteral("'%1' must be an array of strings").arg(key));
        }
        const QJsonArray a = v.toArray();
        for (int i = 0; i < a.size(); ++i) {
            if (!a.at(i).isString()) {
                json::throwBadParam(json::indexPath(key, i), QStringLiteral("'%1' must be an array of strings").arg(key));
            }
            out << a.at(i).toString();
        }
        return out;
    };
    for (const QString& name : stringList(QStringLiteral("type"))) {
        const QString type = checkTypeName(name, QStringLiteral("type"));
        if (!filter.types.contains(type)) {
            filter.types.append(type);
        }
    }
    for (const QString& name : stringList(QStringLiteral("layer"))) {
        RS_Layer* layer = LC_RemoteApiEntityIndex::requireLayer(graphic, name);
        if (!filter.layers.contains(layer)) {
            filter.layers.append(layer);
        }
    }
    const QJsonValue bbox = body.value(QStringLiteral("bbox"));
    if (!bbox.isUndefined() && !bbox.isNull()) {
        QList<double> numbers;
        if (bbox.isArray()) {
            for (const QJsonValue& v : bbox.toArray()) {
                numbers.append(v.isDouble() ? v.toDouble() : std::nan(""));
            }
        }
        filter.box = boxFromNumbers(numbers, QStringLiteral("bbox"));
    }
    const QJsonValue mode = body.value(QStringLiteral("bbox_mode"));
    if (!mode.isUndefined() && !mode.isNull()) {
        filter.crossing = parseBoxMode(mode.toString(), QStringLiteral("bbox_mode"));
    }
    return filter;
}

// --- index -------------------------------------------------------------------

LC_RemoteApiEntityIndex::LC_RemoteApiEntityIndex(RS_Document* document)
    : m_document(document) {
    if (m_document == nullptr) {
        return;
    }
    for (RS_Entity* e : *m_document) {
        if (isLive(e, m_document)) {
            m_entities.append(e);
        }
    }
    QSet<unsigned long long> ownerLineages;
    for (const RS_Entity* e : std::as_const(m_entities)) {
        if (e->ownerLineage() != 0) {
            ownerLineages.insert(e->ownerLineage());
        }
    }
    for (RS_Entity* e : std::as_const(m_entities)) {
        const unsigned long long id = e->lineage();
        if (!m_byId.contains(id)) {
            m_byId.insert(id, e);
            continue;
        }
        // A later live entity with the lineage of an earlier one: a plain copy.
        const bool renewable = e->rtti() != RS2::EntityInsert && e->ownerLineage() == 0 && !ownerLineages.contains(id);
        if (renewable) {
            e->setLineage(RS_Entity::newLineage());
            m_byId.insert(e->lineage(), e);
            ++m_normalized;
        }
        else {
            m_ambiguous.insert(id);
        }
    }
}

RS_Layer* LC_RemoteApiEntityIndex::requireLayer(RS_Graphic* graphic, const QString& name) {
    RS_Layer* layer = graphic != nullptr ? graphic->findLayer(name) : nullptr;
    if (layer == nullptr) {
        throw LC_ApiError(404, QStringLiteral("unknown_layer"), QStringLiteral("no layer named '%1'").arg(name),
                          {{QStringLiteral("name"), name}});
    }
    return layer;
}

bool LC_RemoteApiEntityIndex::isLive(const RS_Entity* e, const RS_Document* document) {
    return e != nullptr && !e->isDeleted() && e->getParent() == document;
}

RS_Entity* LC_RemoteApiEntityIndex::find(const unsigned long long id) const {
    if (m_ambiguous.contains(id)) {
        return nullptr;
    }
    return m_byId.value(id, nullptr);
}

RS_Entity* LC_RemoteApiEntityIndex::require(const unsigned long long id) const {
    return requireAll({id}).constFirst();
}

QList<RS_Entity*> LC_RemoteApiEntityIndex::requireAll(const QList<unsigned long long>& ids) const {
    QList<unsigned long long> unknown;
    QList<unsigned long long> ambiguous;
    QList<RS_Entity*> found;
    for (const unsigned long long id : ids) {
        if (m_ambiguous.contains(id)) {
            ambiguous.append(id);
            continue;
        }
        RS_Entity* e = m_byId.value(id, nullptr);
        if (e == nullptr) {
            unknown.append(id);
            continue;
        }
        if (!found.contains(e)) {
            found.append(e);
        }
    }
    if (!unknown.isEmpty()) {
        throw LC_ApiError(404, QStringLiteral("unknown_entity"),
                          unknown.size() == 1 ? QStringLiteral("no entity with id %1").arg(unknown.constFirst())
                                              : QStringLiteral("%1 of the ids name no entity").arg(unknown.size()),
                          {{QStringLiteral("ids"), idsArray(unknown)}});
    }
    if (!ambiguous.isEmpty()) {
        throw LC_ApiError(409, QStringLiteral("ambiguous_id"),
                          QStringLiteral("more than one entity has this id (a block reference or attribute copied without a new identity)"),
                          {{QStringLiteral("ids"), idsArray(ambiguous)},
                           {QStringLiteral("hint"), QStringLiteral("select the entities by area or layer instead")}});
    }
    return found;
}

bool LC_RemoteApiEntityIndex::matchesBox(const RS_Entity* e, const RS_Vector& corner1, const RS_Vector& corner2, const bool crossing) {
    if (e == nullptr) {
        return false;
    }
    if (e->isInWindow(corner1, corner2)) {
        return true;
    }
    if (!crossing) {
        return false;
    }
    const RS_Vector boxMin(std::min(corner1.x, corner2.x), std::min(corner1.y, corner2.y));
    const RS_Vector boxMax(std::max(corner1.x, corner2.x), std::max(corner1.y, corner2.y));
    if (!e->hasValidBorders() || !boxesOverlap(e->getMin(), e->getMax(), boxMin, boxMax)) {
        return false;
    }
    RS_EntityContainer edges(nullptr, true);
    edges.addRectangle(corner1, corner2);
    if (e->isContainer()) {
        const auto* container = static_cast<const RS_EntityContainer*>(e);
        for (const RS_Entity* child : lc::LC_ContainerTraverser{*container, RS2::ResolveAll}.entities()) {
            if (child != nullptr && touchesEdges(child, edges, corner1, corner2)) {
                return true;
            }
        }
        return false;
    }
    return touchesEdges(e, edges, corner1, corner2);
}

bool LC_RemoteApiEntityIndex::matches(const RS_Entity* e, const LC_RemoteApiEntityFilter& filter) {
    if (!filter.includeHidden && !e->isVisible()) {
        return false;
    }
    if (filter.selectedOnly && !e->isSelected()) {
        return false;
    }
    if (!filter.types.isEmpty() && !filter.types.contains(json::typeName(e))) {
        return false;
    }
    if (!filter.layers.isEmpty() && !filter.layers.contains(e->getLayer(true))) {
        return false;
    }
    if (filter.box.has_value() && !matchesBox(e, filter.box->first, filter.box->second, filter.crossing)) {
        return false;
    }
    return true;
}

QList<RS_Entity*> LC_RemoteApiEntityIndex::query(const LC_RemoteApiEntityFilter& filter) const {
    QList<RS_Entity*> out;
    for (RS_Entity* e : m_entities) {
        if (matches(e, filter)) {
            out.append(e);
        }
    }
    return out;
}

// --- paging and query parameters ---------------------------------------------

LC_RemoteApiPage LC_RemoteApiPage::fromQuery(const QMultiHash<QString, QString>& query) {
    LC_RemoteApiPage page;
    page.limit = LC_RemoteApiQuery::integer(query, QStringLiteral("limit"), 100, 1, MaxLimit);
    page.offset = LC_RemoteApiQuery::integer(query, QStringLiteral("offset"), 0, 0, 1 << 30);
    return page;
}

namespace LC_RemoteApiQuery {

void allowOnly(const QMultiHash<QString, QString>& query, const QStringList& allowed) {
    for (auto it = query.cbegin(); it != query.cend(); ++it) {
        if (it.key() != QLatin1String("doc") && !allowed.contains(it.key())) {
            json::throwUnknownField(it.key(), QStringLiteral("unknown query parameter '%1'").arg(it.key()));
        }
    }
}

std::optional<QString> single(const QMultiHash<QString, QString>& query, const QString& key) {
    const QStringList values = query.values(key);
    if (values.isEmpty()) {
        return std::nullopt;
    }
    if (values.size() > 1) {
        json::throwBadParam(key, QStringLiteral("'%1' is given more than once").arg(key));
    }
    return values.constFirst();
}

bool flag(const QMultiHash<QString, QString>& query, const QString& key, const bool def) {
    const auto value = single(query, key);
    if (!value.has_value()) {
        return def;
    }
    const QString v = value->trimmed().toLower();
    if (v == QLatin1String("true") || v == QLatin1String("1")) {
        return true;
    }
    if (v == QLatin1String("false") || v == QLatin1String("0")) {
        return false;
    }
    json::throwBadParam(key, QStringLiteral("'%1' must be true or false").arg(key),
                        QJsonArray{QStringLiteral("true"), QStringLiteral("false")});
}

int integer(const QMultiHash<QString, QString>& query, const QString& key, const int def, const int min, const int max) {
    const auto value = single(query, key);
    if (!value.has_value()) {
        return def;
    }
    bool ok = false;
    const int n = value->trimmed().toInt(&ok);
    if (!ok || n < min || n > max) {
        json::throwBadParam(key, QStringLiteral("'%1' must be an integer from %2 to %3").arg(key).arg(min).arg(max));
    }
    return n;
}

} // namespace LC_RemoteApiQuery
