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

#include "lc_remoteapiappaccess.h"

#include <QApplication>
#include <QList>
#include <QMdiArea>
#include <QPointer>
#include <QTabBar>

#include "qc_applicationwindow.h"
#include "qc_mdiwindow.h"
#include "qg_actionhandler.h"
#include "qg_graphicview.h"
#include "lc_remoteapitypes.h"
#include "qg_recentfiles.h"
#include "rs_graphic.h"
#include "rs_settings.h"

namespace {

QList<QC_MDIWindow*> documentWindows(const QC_ApplicationWindow* app) {
    QList<QC_MDIWindow*> windows;
    app->doForEachWindow([&windows](QC_MDIWindow* window) {
        windows.append(window);
    });
    return windows;
}

QC_MDIWindow* addedWindow(const QC_ApplicationWindow* app, const QList<QC_MDIWindow*>& before) {
    for (QC_MDIWindow* window : documentWindows(app)) {
        if (!before.contains(window)) {
            return window;
        }
    }
    return nullptr;
}

} // namespace

QC_MDIWindow* LC_RemoteApiAppAccess::newDocument(QC_ApplicationWindow* app, bool* templateFailed) {
    if (templateFailed != nullptr) {
        *templateFailed = false;
    }
    const QList<QC_MDIWindow*> before = documentWindows(app);
    // as slotFileNewFromDefaultTemplate()
    const QString templateFileName = LC_GET_ONE_STR("Paths", "Template", "");
    if (app->newDrawingFromTemplate(templateFileName)) {
        return addedWindow(app, before);
    }
    // newDrawingFromTemplate() leaves the window it made, not activated, when
    // the template cannot be read: close it and start an empty drawing.
    if (QC_MDIWindow* failed = addedWindow(app, before)) {
        close(app, failed);
    }
    if (templateFailed != nullptr) {
        *templateFailed = true;
    }
    app->newDrawingFromTemplate(QString());
    return addedWindow(app, before);
}

QC_MDIWindow* LC_RemoteApiAppAccess::openDocument(QC_ApplicationWindow* app, const QString& path,
                                                  const RS2::FormatType type,
                                                  std::function<bool(bool, const QString&)> onImportError) {
    // librecad-mcp: mirrors QC_ApplicationWindow::openFile(fileName, type), with
    // onImportError in place of its message boxes. Keep the two in step.
    // Unlike openFile, the events pumped here leave out the user's input: this
    // runs inside an API task, where a dialog the user opens would be dismissed
    // by the modal guard, and a window the user closes would be freed under it.
    // The window is still watched, should something else close it.
    const QPointer<QC_MDIWindow> w = app->createNewDrawingWindow(nullptr, path);
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents, 1000);
    const auto closedMeanwhile = [] {
        return LC_ApiError(409, QStringLiteral("document_closed"),
                           QStringLiteral("the new drawing's window was closed while the file loaded"),
                           {{QStringLiteral("hint"), QStringLiteral("open the file again")}});
    };
    if (w.isNull()) {
        throw closedMeanwhile();
    }

    // LC_DocumentsStorage::loadDocument pumps events too (without user input, inside an API task)
    const bool loaded = w->loadDocument(path, type, RS2::DxfTextEncoding::Declared, std::move(onImportError));
    if (w.isNull()) {
        throw closedMeanwhile();
    }
    if (!loaded) {
        // file opening failed, clean up QC_MDIWindow and QMdiSubWindow, without asking
        close(app, w);
        return nullptr;
    }

    // update recent files menu:
    app->m_recentFilesList->add(path);
    app->m_openedFiles.push_back(path);

    if (app->m_mdiAreaCAD->viewMode() == QMdiArea::TabbedView) {
        const QList<QTabBar*> tabBarList = app->m_mdiAreaCAD->findChildren<QTabBar*>();
        QTabBar* tabBar = tabBarList.isEmpty() ? nullptr : tabBarList.at(0);
        if (tabBar != nullptr) {
            tabBar->setExpanding(false);
            tabBar->setTabToolTip(tabBar->currentIndex(), path);
        }
    }
    else {
        app->doArrangeWindows(RS2::CurrentMode);
    }

    app->doActivate(w);
    app->updateWidgetsAsDocumentLoaded(w);

    const auto graphicView = w->getGraphicView();
    app->autoZoomAfterLoad(graphicView);
    RS_Graphic* graphic = graphicView->getGraphic(true);
    if (graphic != nullptr) {
        graphic->setModified(false);
    }
    const int blocksWithGeometry = app->maybeSurfaceBlocksDock(graphic);
    QString message = QC_ApplicationWindow::tr("Loaded document: ") + path;
    if (blocksWithGeometry > 0) {
        message = QC_ApplicationWindow::tr("Loaded %1 — modelspace is empty; %n block(s) in the Blocks "
                                           "dock contain geometry.", "", blocksWithGeometry).arg(path);
    }
    app->showStatusMessage(message);
    return w;
}

void LC_RemoteApiAppAccess::afterSaved(QC_ApplicationWindow* app, QC_MDIWindow* window) {
    // as QC_ApplicationWindow::doSave() after a save, and slotFileSave() after doSave()
    const QString path = window->getFileName();
    app->m_recentFilesList->addIfAbsent(path);
    const auto graphicView = window->getGraphicView();
    app->setupMDIWindowTitleByFile(window, path, graphicView->isDraftMode(), graphicView->isPrintPreview());
    app->startAutoSaveTimer(LC_GET_ONE_BOOL("Defaults", "AutoBackupDocument", true));
    window->setWindowModified(window->getDocument()->isModified());
    app->m_recentFilesList->updateRecentFilesMenu();
}

void LC_RemoteApiAppAccess::close(QC_ApplicationWindow* app, QC_MDIWindow* window) {
    // closeEvent() asks Save/Discard for a modified drawing unless told not to
    window->setSaveOnClosePolicy(QC_MDIWindow::DONT_SAVE);
    app->doClose(window);
    app->doArrangeWindows(RS2::CurrentMode);
}

void LC_RemoteApiAppAccess::activate(QC_ApplicationWindow* app, QC_MDIWindow* window) {
    if (window != nullptr && app->getCurrentMDIWindow() != window) {
        app->doActivate(window);
    }
}

QG_CommandWidget* LC_RemoteApiAppAccess::commandWidget(QC_ApplicationWindow* app) {
    return app->m_commandWidget;
}

QG_ActionHandler* LC_RemoteApiAppAccess::actionHandler(QC_ApplicationWindow* app) {
    return app->m_actionHandler.get();
}
