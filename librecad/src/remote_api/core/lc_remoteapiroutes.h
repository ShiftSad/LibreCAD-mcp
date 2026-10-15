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

#ifndef LC_REMOTEAPIROUTES_H
#define LC_REMOTEAPIROUTES_H

// Route groups of the Remote API components. The controller calls each one
// only when its component is built: LC_REMOTE_API_HAS_ENTITIES and
// LC_REMOTE_API_HAS_DOCUMENTS come from the component's .cmake/.pri fragment.

class LC_RemoteApiRouter;

void lcRegisterEntityRoutes(LC_RemoteApiRouter&);           // remote_api/entities: entities, selection, measure, layers, blocks
void lcRegisterDocumentRoutes(LC_RemoteApiRouter&);         // remote_api/documents: formats, documents, drawing, render, view, undo, command

#endif
