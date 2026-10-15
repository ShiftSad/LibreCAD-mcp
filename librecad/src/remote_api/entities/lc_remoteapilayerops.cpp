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

#include "lc_remoteapilayerops.h"

#include <memory>
#include <vector>

#include <QHash>

#include "lc_graphicviewport.h"
#include "lc_remoteapientityfactory.h"
#include "lc_remoteapientityindex.h"
#include "lc_remoteapientityjson.h"
#include "lc_remoteapientityops.h"
#include "lc_remoteapijson.h"
#include "lc_remoteapitypes.h"
#include "rs_block.h"
#include "rs_blocklist.h"
#include "rs_document.h"
#include "rs_entity.h"
#include "rs_graphic.h"
#include "rs_insert.h"
#include "rs_layer.h"
#include "rs_layerlist.h"

namespace {

namespace json = LC_RemoteApiEntityJson;

const QString& layerZero() {
    static const QString name = QStringLiteral("0");
    return name;
}

QString plural(const int n, const char* one, const char* many) {
    return n == 1 ? QStringLiteral("1 %1").arg(QLatin1String(one)) : QStringLiteral("%1 %2").arg(n).arg(QLatin1String(many));
}

void refuseFrozen(const RS_Layer* layer) {
    throw LC_ApiError(409, QStringLiteral("layer_frozen"), QStringLiteral("layer '%1' is frozen and cannot be the current layer").arg(layer->getName()),
                      {{QStringLiteral("layer"), layer->getName()},
                       {QStringLiteral("hint"), QStringLiteral("thaw it first (update_layer frozen=false)")}});
}

} // namespace

LC_RemoteApiLayerOps::LC_RemoteApiLayerOps(RS_Document* document, LC_GraphicViewport* viewport)
    : m_document(document)
    , m_graphic(document != nullptr ? document->getGraphic() : nullptr)
    , m_viewport(viewport) {
    if (m_graphic == nullptr || m_viewport == nullptr) {
        throw LC_ApiError(500, QStringLiteral("internal"), QStringLiteral("the document has no layers or no view"));
    }
}

void LC_RemoteApiLayerOps::checkLayerName(const QString& name, const QString& param) {
    static const QString forbidden = QStringLiteral("<>/\\\":;?*|,=`");
    if (name.trimmed().isEmpty()) {
        json::throwBadParam(param, QStringLiteral("a layer name must not be empty"));
    }
    for (const QChar c : name) {
        if (forbidden.contains(c)) {
            json::throwBadParam(param, QStringLiteral("a layer name must not contain any of %1").arg(forbidden));
        }
    }
}

int LC_RemoteApiLayerOps::countEntities(const RS_Layer* layer) const {
    int count = 0;
    for (const RS_Entity* e : *m_document) {
        if (LC_RemoteApiEntityIndex::isLive(e, m_document) && e->getLayer(true) == layer) {
            ++count;
        }
    }
    return count;
}

QJsonObject LC_RemoteApiLayerOps::layerJson(RS_Layer* layer) const {
    const RS_Pen pen = layer->getPen();
    const int aci = json::aciNumber(pen.getColor());
    return {{QStringLiteral("name"), layer->getName()},
            {QStringLiteral("color"), json::colorName(pen.getColor())},
            {QStringLiteral("aci"), aci > 0 ? QJsonValue(aci) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("linetype"), json::lineTypeName(pen)},
            {QStringLiteral("lineweight"), json::lineWidthValue(pen.getWidth())},
            {QStringLiteral("frozen"), layer->isFrozen()},
            {QStringLiteral("locked"), layer->isLocked()},
            {QStringLiteral("printable"), layer->isPrint()},
            {QStringLiteral("construction"), layer->isConstruction()},
            {QStringLiteral("current"), layer == m_graphic->getActiveLayer()},
            {QStringLiteral("entity_count"), countEntities(layer)}};
}

QJsonObject LC_RemoteApiLayerOps::list() const {
    QJsonArray layers;
    RS_LayerList* layerList = m_graphic->getLayerList();
    for (RS_Layer* layer : *layerList) {
        if (layer != nullptr) {
            layers.append(layerJson(layer));
        }
    }
    const RS_Layer* current = m_graphic->getActiveLayer();
    return {{QStringLiteral("layers"), layers},
            {QStringLiteral("current"), current != nullptr ? QJsonValue(current->getName()) : QJsonValue(QJsonValue::Null)}};
}

QJsonObject LC_RemoteApiLayerOps::create(const LC_JsonReader& body, QString& audit) const {
    body.allowOnly({"name", "color", "linetype", "lineweight", "frozen", "locked", "printable", "construction", "make_current"});
    const QJsonObject raw = body.raw();
    const QString name = body.str("name");
    checkLayerName(name, QStringLiteral("name"));
    // checked first: RS_LayerList::add() would merge into the existing layer
    if (m_graphic->findLayer(name) != nullptr) {
        throw LC_ApiError(409, QStringLiteral("layer_exists"), QStringLiteral("a layer named '%1' exists").arg(name),
                          {{QStringLiteral("name"), name}});
    }
    auto layer = std::make_unique<RS_Layer>(name);
    RS_Pen pen = layer->getPen();
    if (raw.contains(QStringLiteral("color"))) {
        pen.setColor(json::parseColor(raw.value(QStringLiteral("color")), QStringLiteral("color"), false));
    }
    if (raw.contains(QStringLiteral("linetype"))) {
        pen.setLineType(json::parseLineType(raw.value(QStringLiteral("linetype")), QStringLiteral("linetype"), false));
    }
    if (raw.contains(QStringLiteral("lineweight"))) {
        pen.setWidth(json::parseLineWidth(raw.value(QStringLiteral("lineweight")), QStringLiteral("lineweight"), false));
    }
    layer->setPen(pen);
    const bool frozen = body.flag("frozen", false);
    const bool makeCurrent = body.flag("make_current", false);
    if (makeCurrent && frozen) {
        refuseFrozen(layer.get());
    }
    layer->freeze(frozen);
    layer->lock(body.flag("locked", false));
    layer->setPrint(body.flag("printable", true));
    layer->setConstruction(body.flag("construction", false));

    RS_Layer* added = layer.release();
    m_graphic->addLayer(added);
    if (makeCurrent) {
        m_graphic->activateLayer(added, true);
    }
    m_viewport->notifyChanged();
    audit = QStringLiteral("[MCP] created layer '%1'").arg(name);
    return {{QStringLiteral("layer"), layerJson(added)}, {QStringLiteral("undoable"), false}};
}

QJsonObject LC_RemoteApiLayerOps::update(const QString& name, const LC_JsonReader& body, QJsonArray& warnings, QString& audit) const {
    body.allowOnly({"new_name", "color", "linetype", "lineweight", "frozen", "locked", "printable", "construction"});
    const QJsonObject raw = body.raw();
    RS_Layer* layer = LC_RemoteApiEntityIndex::requireLayer(m_graphic, name);
    if (raw.isEmpty()) {
        json::throwBadParam(QStringLiteral("body"), QStringLiteral("give at least one field to change"));
    }
    RS_Layer edited(*layer);
    bool renamed = false;
    if (raw.contains(QStringLiteral("new_name"))) {
        const QString newName = body.str("new_name");
        checkLayerName(newName, QStringLiteral("new_name"));
        if (newName != layer->getName()) {
            if (layer->getName() == layerZero()) {
                throw LC_ApiError(422, QStringLiteral("cannot_rename_layer_0"), QStringLiteral("layer '0' cannot be renamed"));
            }
            const RS_Layer* existing = m_graphic->findLayer(newName);
            if (existing != nullptr && existing != layer) {
                throw LC_ApiError(409, QStringLiteral("layer_exists"), QStringLiteral("a layer named '%1' exists").arg(newName),
                                  {{QStringLiteral("name"), newName}});
            }
            edited.setName(newName);
            renamed = true;
        }
    }
    RS_Pen pen = edited.getPen();
    if (raw.contains(QStringLiteral("color"))) {
        pen.setColor(json::parseColor(raw.value(QStringLiteral("color")), QStringLiteral("color"), false));
    }
    if (raw.contains(QStringLiteral("linetype"))) {
        pen.setLineType(json::parseLineType(raw.value(QStringLiteral("linetype")), QStringLiteral("linetype"), false));
    }
    if (raw.contains(QStringLiteral("lineweight"))) {
        pen.setWidth(json::parseLineWidth(raw.value(QStringLiteral("lineweight")), QStringLiteral("lineweight"), false));
    }
    edited.setPen(pen);
    if (raw.contains(QStringLiteral("printable"))) {
        edited.setPrint(body.flag("printable", true));
    }
    if (raw.contains(QStringLiteral("construction"))) {
        edited.setConstruction(body.flag("construction", false));
    }
    const bool hasFrozen = raw.contains(QStringLiteral("frozen"));
    const bool frozen = body.flag("frozen", layer->isFrozen());
    const bool hasLocked = raw.contains(QStringLiteral("locked"));
    const bool locked = body.flag("locked", layer->isLocked());
    const QString oldName = layer->getName();

    m_graphic->editLayer(layer, edited);
    // freeze and lock through the graphic: they keep the current layer visible and the selection valid
    if (hasFrozen && frozen != layer->isFrozen()) {
        const bool wasCurrent = layer == m_graphic->getActiveLayer();
        if (frozen) {
            m_graphic->setFreezeLayers({}, {layer});
        }
        else {
            m_graphic->setFreezeLayers({layer}, {});
        }
        if (frozen && wasCurrent) {
            const RS_Layer* current = m_graphic->getActiveLayer();
            warnings.append(QStringLiteral("layer '%1' was the current layer; the current layer is now '%2'")
                                .arg(layer->getName(), current != nullptr ? current->getName() : QString()));
        }
    }
    if (hasLocked && locked != layer->isLocked()) {
        if (locked) {
            m_graphic->setLockLayers({}, {layer});
        }
        else {
            m_graphic->setLockLayers({layer}, {});
        }
    }
    if (renamed) {
        // as RS_ActionLayersEdit: entities show their layer's name in their caches
        for (RS_Entity* e : *m_document) {
            if (LC_RemoteApiEntityIndex::isLive(e, m_document) && e->getLayer(true) == layer) {
                e->update();
            }
        }
    }
    m_graphic->setModified(true);
    m_viewport->notifyChanged();
    audit = renamed ? QStringLiteral("[MCP] renamed layer '%1' to '%2'").arg(oldName, layer->getName())
                    : QStringLiteral("[MCP] changed layer '%1'").arg(layer->getName());
    return {{QStringLiteral("layer"), layerJson(layer)}, {QStringLiteral("undoable"), false}};
}

QJsonObject LC_RemoteApiLayerOps::setCurrent(const QString& name) const {
    RS_Layer* layer = LC_RemoteApiEntityIndex::requireLayer(m_graphic, name);
    if (layer->isFrozen()) {
        refuseFrozen(layer);
    }
    m_graphic->activateLayer(layer, true);
    return {{QStringLiteral("current"), layer->getName()}};
}

QJsonObject LC_RemoteApiLayerOps::remove(const QString& name, const QMultiHash<QString, QString>& query, QString& audit) const {
    LC_RemoteApiQuery::allowOnly(query, {QStringLiteral("entities"), QStringLiteral("move_to")});
    const QString policy = LC_RemoteApiQuery::single(query, QStringLiteral("entities")).value_or(QStringLiteral("refuse"));
    if (policy != QLatin1String("refuse") && policy != QLatin1String("delete") && policy != QLatin1String("move")) {
        json::throwBadParam(QStringLiteral("entities"), QStringLiteral("'entities' must be refuse, delete or move"),
                            QJsonArray{QStringLiteral("refuse"), QStringLiteral("delete"), QStringLiteral("move")});
    }
    const auto moveToValue = LC_RemoteApiQuery::single(query, QStringLiteral("move_to"));
    if (moveToValue.has_value() && policy != QLatin1String("move")) {
        json::throwBadParam(QStringLiteral("move_to"), QStringLiteral("'move_to' applies to entities=move only"));
    }
    RS_Layer* layer = LC_RemoteApiEntityIndex::requireLayer(m_graphic, name);
    if (layer->getName() == layerZero()) {
        throw LC_ApiError(422, QStringLiteral("cannot_delete_layer_0"), QStringLiteral("layer '0' cannot be deleted"));
    }
    QList<RS_Entity*> entities;
    for (RS_Entity* e : *m_document) {
        if (LC_RemoteApiEntityIndex::isLive(e, m_document) && e->getLayer(true) == layer) {
            entities.append(e);
        }
    }
    const int count = static_cast<int>(entities.size());
    if (count > 0 && policy == QLatin1String("refuse")) {
        throw LC_ApiError(409, QStringLiteral("layer_not_empty"), QStringLiteral("layer '%1' holds %2").arg(layer->getName(), plural(count, "entity", "entities")),
                          {{QStringLiteral("entity_count"), count},
                           {QStringLiteral("hint"), QStringLiteral("pass entities=delete or entities=move (with move_to)")}});
    }
    RS_Layer* layerZeroPtr = LC_RemoteApiEntityIndex::requireLayer(m_graphic, layerZero());
    RS_Layer* target = nullptr;
    if (policy == QLatin1String("move")) {
        target = LC_RemoteApiEntityIndex::requireLayer(m_graphic, moveToValue.value_or(layerZero()));
        if (target == layer) {
            json::throwBadParam(QStringLiteral("move_to"), QStringLiteral("'move_to' must name another layer"));
        }
        if (target->isLocked() && count > 0) {
            throw LC_ApiError(409, QStringLiteral("layer_locked"), QStringLiteral("layer '%1' is locked").arg(target->getName()),
                              {{QStringLiteral("layer"), target->getName()}});
        }
    }
    if (layer->isLocked() && count > 0) {
        // LC_UndoSection would silently keep the entities of a locked layer
        throw LC_ApiError(409, QStringLiteral("layer_locked"),
                          QStringLiteral("layer '%1' is locked; unlock it before deleting it with its entities").arg(layer->getName()),
                          {{QStringLiteral("layer"), layer->getName()}});
    }
    const QString layerName = layer->getName();
    if (layer == m_graphic->getActiveLayer()) {
        m_graphic->activateLayer(layerZeroPtr, true);
    }

    int moved = 0;
    int deleted = 0;
    if (count > 0 && target != nullptr) {
        // undoable: the entities go to the target layer in one undo step
        m_document->undoableModify(m_viewport, [&](LC_DocumentModificationBatch& ctx) {
            ctx.dontSetActiveLayerAndPen();
            for (RS_Entity* e : std::as_const(entities)) {
                RS_Entity* clone = e->clone();
                clone->setLayer(target);
                ctx.replace(e, clone);
            }
            return true;
        });
        moved = count;
    }
    else if (count > 0) {
        // undoable as in RS_Graphic::removeLayer: an undo brings them back on layer "0" (see below)
        m_document->undoableModify(m_viewport, [&](LC_DocumentModificationBatch& ctx) {
            ctx.remove(entities);
            return true;
        });
        deleted = count;
    }
    // The undo history must not point at the layer once it is freed: what an
    // undo brings back lands on the target layer (or "0").
    RS_Layer* fallback = target != nullptr ? target : layerZeroPtr;
    for (RS_Entity* e : *m_document) {
        if (e != nullptr && e->isDeleted() && e->getLayer(false) == layer) {
            e->setLayer(fallback);
        }
    }
    m_graphic->removeLayer(layer);
    m_viewport->notifyChanged();

    audit = QStringLiteral("[MCP] deleted layer '%1'").arg(layerName);
    if (moved > 0) {
        audit += QStringLiteral(" and moved %1 to '%2'").arg(plural(moved, "entity", "entities"), target->getName());
    }
    else if (deleted > 0) {
        audit += QStringLiteral(" with %1").arg(plural(deleted, "entity", "entities"));
    }
    return {{QStringLiteral("deleted"), layerName},
            {QStringLiteral("entities_deleted"), deleted},
            {QStringLiteral("entities_moved"), moved},
            {QStringLiteral("undoable"), false}};
}

QJsonObject LC_RemoteApiLayerOps::listBlocks() const {
    QHash<QString, int> insertCounts;
    for (const RS_Entity* e : *m_document) {
        if (LC_RemoteApiEntityIndex::isLive(e, m_document) && e->rtti() == RS2::EntityInsert) {
            insertCounts[static_cast<const RS_Insert*>(e)->getName()]++;
        }
    }
    QJsonArray blocks;
    const RS_BlockList* blockList = m_graphic->getBlockList();
    for (RS_Block* block : *blockList) {
        if (block == nullptr) {
            continue;
        }
        int entityCount = 0;
        for (const RS_Entity* e : *block) {
            if (e != nullptr && !e->isDeleted()) {
                ++entityCount;
            }
        }
        blocks.append(QJsonObject{{QStringLiteral("name"), block->getName()},
                                  {QStringLiteral("base_point"), json::pointValue(block->getBasePoint())},
                                  {QStringLiteral("entity_count"), entityCount},
                                  {QStringLiteral("frozen"), block->isFrozen()},
                                  {QStringLiteral("insert_count"), insertCounts.value(block->getName(), 0)}});
    }
    return {{QStringLiteral("blocks"), blocks}};
}

QJsonObject LC_RemoteApiLayerOps::insertBlock(const QString& name, const LC_JsonReader& body, QJsonArray& warnings, QString& audit) const {
    body.allowOnly({"position", "scale", "angle_deg", "layer", "columns", "rows", "spacing", "select"});
    if (m_graphic->findBlock(name) == nullptr) {
        throw LC_ApiError(404, QStringLiteral("unknown_block"), QStringLiteral("no block named '%1'").arg(name), {{QStringLiteral("name"), name}});
    }
    QJsonObject spec = body.raw();
    spec.remove(QStringLiteral("layer"));
    spec.remove(QStringLiteral("select"));
    spec.insert(QStringLiteral("type"), QStringLiteral("insert"));
    spec.insert(QStringLiteral("block"), name);

    const LC_RemoteApiEntityOps ops(m_document, m_viewport);
    RS_Layer* layer = ops.targetLayer(body.str("layer", QString()));
    const LC_RemoteApiEntityFactory factory(m_document, nullptr);
    QStringList notes;
    std::vector<std::unique_ptr<RS_Entity>> built;
    built.push_back(factory.create(spec, QString(), notes));
    for (const QString& note : std::as_const(notes)) {
        warnings.append(note);
    }
    const QList<RS_Entity*> added = ops.addEntities(built, {layer}, {RS_Pen(RS_Color(RS2::FlagByLayer), RS2::WidthByLayer, RS2::LineByLayer)},
                                                    body.flag("select", false), warnings);
    if (added.isEmpty()) {
        throw LC_ApiError(500, QStringLiteral("internal"), QStringLiteral("the block reference was not added"));
    }
    audit = QStringLiteral("[MCP] inserted block '%1' on layer '%2'").arg(name, layer->getName());
    return {{QStringLiteral("entity"), json::entityToJson(added.constFirst(), json::Detail::Full)}, {QStringLiteral("undoable"), true}};
}
