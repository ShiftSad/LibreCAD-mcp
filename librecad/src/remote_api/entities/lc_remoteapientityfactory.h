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

#ifndef LC_REMOTEAPIENTITYFACTORY_H
#define LC_REMOTEAPIENTITYFACTORY_H

// Entity specs of the Remote API (design section 2.6) to entities, and the
// geometry part of PATCH /entities/{id}.

#include <memory>

#include <QJsonObject>
#include <QString>
#include <QStringList>

class LC_RemoteApiEntityIndex;
class RS_Document;
class RS_Entity;
class RS_Graphic;

class LC_RemoteApiEntityFactory {
public:
    /** index resolves boundary_ids of hatches; it may be nullptr when no spec uses them. */
    LC_RemoteApiEntityFactory(RS_Document* document, const LC_RemoteApiEntityIndex* index);

    /**
     * Builds the entity of one spec, with the document as parent but not added
     * to it, and with the geometry only: layer and pen are the caller's.
     * path locates the spec in the request ("entities[2]"); errors name it.
     *
     * Throws LC_ApiError:
     * - 400 missing_param, bad_param, unknown_field for the shape of the spec;
     * - 422 invalid_entity{errors} for geometry (radius 0, too few vertices, ...);
     * - 422 invalid_hatch_boundary when a hatch boundary does not close;
     * - 404 unknown_block, unknown_entity for what the spec refers to.
     * warnings gets notes that do not stop the creation ("style: ...").
     */
    std::unique_ptr<RS_Entity> create(const QJsonObject& spec, const QString& path, QStringList& warnings) const;

    /**
     * The replacement of original for a PATCH body: the body's geometry fields
     * (attributes and "type" are ignored here) merged over the entity's own.
     * The result keeps the identity of original (lineage, handle, layer, pen,
     * extended data); the caller applies the attributes. With no geometry field
     * it is a plain clone.
     *
     * Throws 400 unknown_field for a field of another type, 400 bad_param for a
     * change of type or a geometry change of a read-only type, and the errors
     * of create() for the merged values.
     */
    std::unique_ptr<RS_Entity> patch(RS_Entity* original, const QJsonObject& body, QStringList& warnings) const;

    /**
     * fresh takes the identity of original: lineage, owner lineage, handles,
     * extended data, flags, layer and pen; then its borders are recomputed.
     */
    static void transplantIdentity(const RS_Entity* original, RS_Entity* fresh);

    /** The fields a spec of the wire type takes (with "type" and the attributes). */
    static QStringList specFields(const QString& typeName, const QString& dimType = {});

private:
    RS_Document* m_document = nullptr;
    RS_Graphic* m_graphic = nullptr;
    const LC_RemoteApiEntityIndex* m_index = nullptr;
};

#endif
