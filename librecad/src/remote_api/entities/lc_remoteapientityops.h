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

#ifndef LC_REMOTEAPIENTITYOPS_H
#define LC_REMOTEAPIENTITYOPS_H

// Entity operations of the Remote API (design sections 2.5 and 3.3): query,
// create, update, delete, transform, selection and measure. GUI thread only.
//
// Every change goes through RS_Document::undoableModify with
// dontSetActiveLayerAndPen(), so one request is one undo step. Only live,
// top-level entities are edited, and locked entities and layers are refused
// before anything changes (LC_UndoSection silently skips deleting an entity
// on a locked layer while still adding its replacement).

#include <memory>
#include <vector>

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMultiHash>
#include <QString>

#include "rs_pen.h"

class LC_GraphicViewport;
class LC_JsonReader;
class RS_Document;
class RS_Entity;
class RS_Graphic;
class RS_Layer;

class LC_RemoteApiEntityOps {
public:
    static constexpr int MaxBatch = 5000;      // entities per create request
    static constexpr int MaxCopies = 100;      // copies of one transform

    /** viewport must be the document's: undo sections need it. */
    LC_RemoteApiEntityOps(RS_Document* document, LC_GraphicViewport* viewport);

    /** GET /entities: {entities, total, offset, limit, next_offset}. */
    QJsonObject list(const QMultiHash<QString, QString>& query) const;
    /** GET /entities/{id}: {entity} in full detail. */
    QJsonObject get(unsigned long long id) const;
    /** POST /entities {entities, layer?, select?}: {created, ids, undoable}. Atomic. */
    QJsonObject create(const LC_JsonReader& body, QJsonArray& warnings, QString& audit) const;
    /** PATCH /entities/{id}: {entity, undoable}, the id unchanged. */
    QJsonObject update(unsigned long long id, const LC_JsonReader& body, QJsonArray& warnings, QString& audit) const;
    /** DELETE /entities/{id}, POST /entities/delete: {deleted, undoable}. All or nothing. */
    QJsonObject remove(const QList<unsigned long long>& ids, QString& audit) const;
    /** POST /entities/transform: {entities, ids, originals_kept, undoable}. */
    QJsonObject transform(const LC_JsonReader& body, QString& audit) const;
    /** GET /selection: {ids, count}. */
    QJsonObject selection() const;
    /** POST /selection {mode, ids?, bbox?, bbox_mode?, type?, layer?}: {ids, count}. */
    QJsonObject select(const LC_JsonReader& body) const;
    /** POST /measure (read-only). */
    QJsonObject measure(const LC_JsonReader& body) const;

    /**
     * Adds built entities (document as parent, not yet added) in one undo
     * step, each with its layer and pen. Checks the layers first: 409
     * layer_locked; a frozen layer adds a "layer_frozen" warning. Returns the
     * added entities. Used by create and by POST /blocks/{name}/insert.
     */
    QList<RS_Entity*> addEntities(std::vector<std::unique_ptr<RS_Entity>>& entities, const QList<RS_Layer*>& layers,
                                  const QList<RS_Pen>& pens, bool select, QJsonArray& warnings) const;

    /** The layer for new entities: the named one (404 unknown_layer) or, with an empty name, the current layer. */
    RS_Layer* targetLayer(const QString& name) const;
    /** "[MCP] created 3 entities on layer 'walls'" */
    static QString describeCreated(const QList<RS_Entity*>& entities);

private:
    RS_Document* m_document = nullptr;
    RS_Graphic* m_graphic = nullptr;
    LC_GraphicViewport* m_viewport = nullptr;
};

#endif
