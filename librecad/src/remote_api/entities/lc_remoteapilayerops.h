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

#ifndef LC_REMOTEAPILAYEROPS_H
#define LC_REMOTEAPILAYEROPS_H

// Layer and block operations of the Remote API (design section 2.5). GUI
// thread only.
//
// LibreCAD cannot undo changes to the layer or block lists, so these
// responses say "undoable": false. Changes to entities they make (moving the
// entities of a deleted layer, inserting a block) are undoable entity edits.

#include <QJsonArray>
#include <QJsonObject>
#include <QMultiHash>
#include <QString>

class LC_GraphicViewport;
class LC_JsonReader;
class RS_Document;
class RS_Graphic;
class RS_Layer;

class LC_RemoteApiLayerOps {
public:
    LC_RemoteApiLayerOps(RS_Document* document, LC_GraphicViewport* viewport);

    /** GET /layers: {layers, current}. */
    QJsonObject list() const;
    /** POST /layers: {layer, undoable:false}. 409 layer_exists, 400 bad_param(name). */
    QJsonObject create(const LC_JsonReader& body, QString& audit) const;
    /** PATCH /layers/{name}: {layer, undoable:false}. 422 cannot_rename_layer_0, 409 layer_exists. */
    QJsonObject update(const QString& name, const LC_JsonReader& body, QJsonArray& warnings, QString& audit) const;
    /** POST /layers/{name}/current: {current}. 409 layer_frozen. */
    QJsonObject setCurrent(const QString& name) const;
    /**
     * DELETE /layers/{name}?entities=refuse|delete|move&move_to=0:
     * {deleted, entities_deleted, entities_moved, undoable:false}.
     * 409 layer_not_empty{entity_count}, 422 cannot_delete_layer_0.
     */
    QJsonObject remove(const QString& name, const QMultiHash<QString, QString>& query, QString& audit) const;

    /** GET /blocks: {blocks: [{name, base_point, entity_count, frozen, insert_count}]}. */
    QJsonObject listBlocks() const;
    /** POST /blocks/{name}/insert: {entity, undoable:true}. 404 unknown_block. */
    QJsonObject insertBlock(const QString& name, const LC_JsonReader& body, QJsonArray& warnings, QString& audit) const;

    /** A layer as the API shows it. */
    QJsonObject layerJson(RS_Layer* layer) const;
    /** 400 bad_param for an empty name or one with <>/\":;?*|,=` */
    static void checkLayerName(const QString& name, const QString& param);

private:
    int countEntities(const RS_Layer* layer) const;

    RS_Document* m_document = nullptr;
    RS_Graphic* m_graphic = nullptr;
    LC_GraphicViewport* m_viewport = nullptr;
};

#endif
