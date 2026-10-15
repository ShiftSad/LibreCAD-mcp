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

#include "lc_remoteapicontext.h"

#include "qc_applicationwindow.h"
#include "qc_mdiwindow.h"
#include "qg_graphicview.h"
#include "rs_actioninterface.h"
#include "rs_document.h"
#include "rs_graphic.h"

namespace {

bool isPrintPreview(const QC_MDIWindow* window) {
    const QG_GraphicView* view = window->getGraphicView();
    return view != nullptr && view->isPrintPreview();
}

// Print previews and block editors belong to the drawing window they were
// opened from; that window is the document.
QC_MDIWindow* topLevel(QC_MDIWindow* window) {
    int guard = 0;
    while (window != nullptr && window->getParentWindow() != nullptr && guard++ < 16) {
        window = window->getParentWindow();
    }
    return window;
}

} // namespace

LC_RemoteApiContext::LC_RemoteApiContext(QC_ApplicationWindow* appWindow)
    : m_appWindow(appWindow) {
}

QC_ApplicationWindow* LC_RemoteApiContext::appWindow() const {
    return m_appWindow;
}

LC_RemoteApiConfig LC_RemoteApiContext::config() const {
    return LC_RemoteApiConfig::load();
}

QList<QC_MDIWindow*> LC_RemoteApiContext::documentWindows() const {
    QList<QC_MDIWindow*> windows;
    if (m_appWindow == nullptr) {
        return windows;
    }
    m_appWindow->doForEachWindow([&windows](QC_MDIWindow* window) {
        if (window != nullptr && window->getParentWindow() == nullptr && !isPrintPreview(window) &&
            window->getDocument() != nullptr) {
            windows << window;
        }
    });
    return windows;
}

QC_MDIWindow* LC_RemoteApiContext::activeDocumentWindow() const {
    if (m_appWindow == nullptr) {
        return nullptr;
    }
    QC_MDIWindow* window = topLevel(m_appWindow->getCurrentMDIWindow());
    if (window != nullptr && documentWindows().contains(window)) {
        return window;
    }
    return nullptr;
}

LC_ResolvedDoc LC_RemoteApiContext::resolve(const LC_ApiRequest& request) const {
    const QString docParam = request.query.value(QStringLiteral("doc")).trimmed();
    if (docParam.isEmpty()) {
        QC_MDIWindow* window = activeDocumentWindow();
        if (window == nullptr) {
            const QList<QC_MDIWindow*> windows = documentWindows();
            if (windows.isEmpty()) {
                throw LC_ApiError(409, QStringLiteral("no_document"), QStringLiteral("no drawing is open"),
                                  {{QStringLiteral("hint"), QStringLiteral("open or create a drawing first")}});
            }
            // documents are open but none is active (all minimised, or the
            // focus is elsewhere): only a single one is unambiguous
            if (windows.size() > 1) {
                throw LC_ApiError(409, QStringLiteral("no_document"), QStringLiteral("no drawing is active"),
                                  {{QStringLiteral("hint"), QStringLiteral("pass ?doc=<id> (see /documents)")}});
            }
            window = windows.first();
        }
        return resolve(window);
    }

    bool ok = false;
    const unsigned id = docParam.toUInt(&ok);
    if (!ok) {
        throw LC_ApiError(400, QStringLiteral("bad_param"), QStringLiteral("doc must be a document id"),
                          {{QStringLiteral("param"), QStringLiteral("doc")}});
    }
    for (QC_MDIWindow* window : documentWindows()) {
        if (window->getId() == id) {
            return resolve(window);
        }
        // a print preview or block editor id addresses its drawing
        for (QC_MDIWindow* child : window->getChildWindows()) {
            if (child != nullptr && child->getId() == id) {
                return resolve(window);
            }
        }
    }
    throw LC_ApiError(404, QStringLiteral("unknown_document"), QStringLiteral("no open drawing has id %1").arg(id),
                      {{QStringLiteral("doc"), static_cast<double>(id)}});
}

LC_ResolvedDoc LC_RemoteApiContext::resolve(QC_MDIWindow* window) const {
    window = topLevel(window);
    if (window == nullptr || window->getDocument() == nullptr) {
        throw LC_ApiError(409, QStringLiteral("no_document"), QStringLiteral("no drawing is open"));
    }
    LC_ResolvedDoc resolved;
    resolved.id = window->getId();
    resolved.window = window;
    resolved.document = window->getDocument();
    resolved.graphic = window->getGraphic();
    resolved.view = window->getGraphicView();
    resolved.viewport = resolved.view != nullptr ? resolved.view->getViewPort() : nullptr;
    return resolved;
}

QString LC_RemoteApiContext::cancelActiveAction(const LC_ResolvedDoc& doc) const {
    if (doc.view == nullptr) {
        return {};
    }
    RS_ActionInterface* current = doc.view->getCurrentAction();
    if (current == nullptr || current == doc.view->getDefaultAction()) {
        return {};
    }
    QString name = doc.view->getCurrentActionName();
    if (name.isEmpty()) {
        name = current->getName();
    }
    if (name.isEmpty()) {
        name = QStringLiteral("action %1").arg(static_cast<int>(current->rtti()));
    }
    doc.view->killAllActions();
    return name;
}

void LC_RemoteApiContext::notify(const QString& line, const bool force) const {
    if (m_appWindow == nullptr || line.isEmpty()) {
        return;
    }
    if (!force && !config().notify) {
        return;
    }
    m_appWindow->commandMessage(line);
}
