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

#ifndef LC_REMOTEAPIGUI_H
#define LC_REMOTEAPIGUI_H

// Runs Remote API work on the GUI thread (design section 1.2).
// Pinned interface (implemented in lc_remoteapigui.cpp by the core component).

#include <functional>

#include <QString>

#include "lc_remoteapitypes.h"

namespace LC_RemoteApiGui {
    // Posts fn to the GUI thread and waits up to timeoutMs. Returns fn's response,
    // or the envelope of an LC_ApiError it threw; 409 gui_modal_open when the user
    // has a modal dialog open; throws LC_ApiError 504 ui_timeout on timeout.
    LC_ApiResponse run(std::function<LC_ApiResponse()> fn, int timeoutMs, const QString& route); // server thread
    int depth();                                            // GUI thread; >0 while an API task runs

    // ---- additions of the core component ----------------------------------

    // Whether a task may run while the user has a modal dialog open. Only
    // read-only status routes use Allow (GET /status still answers then).
    enum class ModalPolicy { Refuse, Allow };
    LC_ApiResponse run(std::function<LC_ApiResponse()> fn, int timeoutMs, const QString& route,
                       ModalPolicy modalPolicy);

    // Called on the GUI thread by the server's stop(): every run() waiting
    // right now returns at once with 503 shutting_down, so the server thread
    // can end while the GUI thread waits for it. Tasks not started yet never run.
    void abortWaiting();

    // GUI thread. Receives the route of a task that finished after its client
    // had already been answered with ui_timeout or shutting_down (the controller
    // writes "[MCP] <route> finished after the client gave up"). Empty clears.
    void setLateTaskNotifier(std::function<void(const QString& route)> notifier);
}

#endif
