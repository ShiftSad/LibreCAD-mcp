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

#ifndef LC_REMOTEAPIENTITYINDEX_H
#define LC_REMOTEAPIENTITYINDEX_H

// Entity ids of the Remote API (design section 2.6) and entity queries.
//
// An id is RS_Entity::lineage(), which survives the clone-and-replace of every
// edit and undo. Only live, top-level entities of the document are addressable:
// not deleted (undo history) and with the document as parent.

#include <optional>
#include <utility>

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMultiHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include "rs_vector.h"

class RS_Document;
class RS_Entity;
class RS_Graphic;
class RS_Layer;

/**
 * Which entities a query or a selection change applies to.
 */
struct LC_RemoteApiEntityFilter {
    QStringList types;                 // wire type names; empty: every type
    QList<RS_Layer*> layers;           // empty: every layer
    std::optional<std::pair<RS_Vector, RS_Vector>> box;   // two corners, any order
    bool crossing = true;              // crossing: in or touching the box; window: fully inside
    bool selectedOnly = false;
    bool includeHidden = false;        // invisible entities and entities on frozen layers

    bool hasConditions() const {
        return !types.isEmpty() || !layers.isEmpty() || box.has_value() || selectedOnly;
    }

    /**
     * GET /entities query parameters: type (repeatable, or one comma-separated
     * value), layer (repeatable, never comma-split), bbox=x1,y1,x2,y2,
     * bbox_mode=crossing|window, selected, include_hidden.
     * Throws 400 bad_param, 404 unknown_layer.
     */
    static LC_RemoteApiEntityFilter fromQuery(const QMultiHash<QString, QString>& query, RS_Graphic* graphic);
    /**
     * Body fields of POST /selection: type: [..], layer: [..], bbox: [x1,y1,x2,y2], bbox_mode.
     */
    static LC_RemoteApiEntityFilter fromBody(const QJsonObject& body, RS_Graphic* graphic);
};

/**
 * The live top-level entities of a document by id, built for one request.
 *
 * Building it normalizes accidental duplicate lineages (a plain copy and paste
 * in the GUI keeps the lineage): of two live entities with one lineage, the
 * later one gets a new lineage, unless it is a block reference, belongs to one
 * (ownerLineage() != 0) or its lineage is the owner lineage of another entity.
 * Such an id stays ambiguous: it is reported, but looking it up gives
 * 409 ambiguous_id.
 */
class LC_RemoteApiEntityIndex {
public:
    explicit LC_RemoteApiEntityIndex(RS_Document* document);

    RS_Document* document() const {
        return m_document;
    }
    /** Live top-level entities in document order. */
    const QList<RS_Entity*>& entities() const {
        return m_entities;
    }
    /** How many duplicate lineages this index renewed. */
    int normalizedCount() const {
        return m_normalized;
    }

    /** nullptr when the id is unknown or ambiguous. */
    RS_Entity* find(unsigned long long id) const;
    bool isAmbiguous(unsigned long long id) const {
        return m_ambiguous.contains(id);
    }
    /** Throws 404 unknown_entity{ids} or 409 ambiguous_id{ids}. */
    RS_Entity* require(unsigned long long id) const;
    /** All or nothing: throws 404 unknown_entity with every unknown id, or 409 ambiguous_id. */
    QList<RS_Entity*> requireAll(const QList<unsigned long long>& ids) const;

    /** Entities matching the filter, in document order. */
    QList<RS_Entity*> query(const LC_RemoteApiEntityFilter& filter) const;

    /** Whether e passes every condition of the filter. */
    static bool matches(const RS_Entity* e, const LC_RemoteApiEntityFilter& filter);
    static bool isLive(const RS_Entity* e, const RS_Document* document);
    /** The layer of that name; throws 404 unknown_layer{name}. */
    static RS_Layer* requireLayer(RS_Graphic* graphic, const QString& name);
    /** Window: the bounding box is inside the box. Crossing: also an entity that touches the box edges. */
    static bool matchesBox(const RS_Entity* e, const RS_Vector& corner1, const RS_Vector& corner2, bool crossing);

private:
    RS_Document* m_document = nullptr;
    QList<RS_Entity*> m_entities;
    QHash<unsigned long long, RS_Entity*> m_byId;
    QSet<unsigned long long> m_ambiguous;
    int m_normalized = 0;
};

/**
 * Paging of GET /entities: limit (1..1000, default 100) and offset (default 0).
 */
struct LC_RemoteApiPage {
    static constexpr int MaxLimit = 1000;
    int limit = 100;
    int offset = 0;

    static LC_RemoteApiPage fromQuery(const QMultiHash<QString, QString>& query);
};

/**
 * Helpers for query parameters (all throw 400 bad_param).
 */
namespace LC_RemoteApiQuery {
    /** Refuses parameters outside allowed ("doc" is always allowed): 400 unknown_field. */
    void allowOnly(const QMultiHash<QString, QString>& query, const QStringList& allowed);
    /** The single value of a parameter; nullopt when absent; 400 when given twice. */
    std::optional<QString> single(const QMultiHash<QString, QString>& query, const QString& key);
    bool flag(const QMultiHash<QString, QString>& query, const QString& key, bool def);
    int integer(const QMultiHash<QString, QString>& query, const QString& key, int def, int min, int max);
}

#endif
