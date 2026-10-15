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

#ifndef LC_REMOTEAPIENTITYROUTES_H
#define LC_REMOTEAPIENTITYROUTES_H

// Handlers of the entities component (design section 2.4, owner B), registered
// by lcRegisterEntityRoutes() in lc_remoteapiroutes_entities.cpp. They run on
// the GUI thread with the document resolved (needsDocument); the tests call
// them directly with LC_ApiCall::setDoc().

#include "lc_remoteapitypes.h"

class LC_ApiCall;

namespace LC_RemoteApiEntityRoutes {
    // layers (not undoable)
    LC_ApiResponse listLayers(LC_ApiCall& call);        // GET    /api/v1/layers
    LC_ApiResponse createLayer(LC_ApiCall& call);       // POST   /api/v1/layers
    LC_ApiResponse updateLayer(LC_ApiCall& call);       // PATCH  /api/v1/layers/{name}
    LC_ApiResponse setCurrentLayer(LC_ApiCall& call);   // POST   /api/v1/layers/{name}/current
    LC_ApiResponse deleteLayer(LC_ApiCall& call);       // DELETE /api/v1/layers/{name}
    // blocks
    LC_ApiResponse listBlocks(LC_ApiCall& call);        // GET    /api/v1/blocks
    LC_ApiResponse insertBlock(LC_ApiCall& call);       // POST   /api/v1/blocks/{name}/insert
    // entities (undoable)
    LC_ApiResponse listEntities(LC_ApiCall& call);      // GET    /api/v1/entities
    LC_ApiResponse getEntity(LC_ApiCall& call);         // GET    /api/v1/entities/{id}
    LC_ApiResponse createEntities(LC_ApiCall& call);    // POST   /api/v1/entities
    LC_ApiResponse updateEntity(LC_ApiCall& call);      // PATCH  /api/v1/entities/{id}
    LC_ApiResponse deleteEntity(LC_ApiCall& call);      // DELETE /api/v1/entities/{id}
    LC_ApiResponse deleteEntities(LC_ApiCall& call);    // POST   /api/v1/entities/delete
    LC_ApiResponse transformEntities(LC_ApiCall& call); // POST   /api/v1/entities/transform
    // selection and measure
    LC_ApiResponse getSelection(LC_ApiCall& call);      // GET    /api/v1/selection
    LC_ApiResponse setSelection(LC_ApiCall& call);      // POST   /api/v1/selection
    LC_ApiResponse measure(LC_ApiCall& call);           // POST   /api/v1/measure
}

#endif
