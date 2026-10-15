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

// TEST-ONLY stand-in for core/lc_remoteapicontext.cpp.
//
// entities.cmake adds it to the test sources only while the core component's
// context is not on the branch; it drops out by itself once the real one
// lands. The entities tests have no main window: they hand the document to
// LC_ApiCall::setDoc(), so resolving one here always fails as with no
// document open.

#include "lc_remoteapicontext.h"

LC_RemoteApiContext::LC_RemoteApiContext(QC_ApplicationWindow* appWindow)
    : m_appWindow(appWindow) {
}

QC_ApplicationWindow* LC_RemoteApiContext::appWindow() const {
    return m_appWindow;
}

LC_RemoteApiConfig LC_RemoteApiContext::config() const {
    return {};
}

QList<QC_MDIWindow*> LC_RemoteApiContext::documentWindows() const {
    return {};
}

QC_MDIWindow* LC_RemoteApiContext::activeDocumentWindow() const {
    return nullptr;
}

LC_ResolvedDoc LC_RemoteApiContext::resolve(const LC_ApiRequest&) const {
    throw LC_ApiError(409, QStringLiteral("no_document"), QStringLiteral("no drawing is open"));
}

LC_ResolvedDoc LC_RemoteApiContext::resolve(QC_MDIWindow*) const {
    throw LC_ApiError(409, QStringLiteral("no_document"), QStringLiteral("no drawing is open"));
}

QString LC_RemoteApiContext::cancelActiveAction(const LC_ResolvedDoc&) const {
    return {};
}

void LC_RemoteApiContext::notify(const QString&, bool) const {
}
