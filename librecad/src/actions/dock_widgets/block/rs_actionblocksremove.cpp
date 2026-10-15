/****************************************************************************
**
** This file is part of the LibreCAD project, a 2D CAD program
**
** Copyright (C) 2010 R. van Twisk (librecad@rvt.dds.nl)
** Copyright (C) 2001-2003 RibbonSoft. All rights reserved.
**
**
** This file may be distributed and/or modified under the terms of the
** GNU General Public License version 2 as published by the Free Software
** Foundation and appearing in the file gpl-2.0.txt included in the
** packaging of this file.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
**
** This copyright notice MUST APPEAR in all copies of the script!
**
**********************************************************************/

#include "rs_actionblocksremove.h"

#include "lc_paperspacestore.h"
#include "rs_dialogfactory.h"
#include "rs_dialogfactoryinterface.h"
#include "rs_graphic.h"
#include "rs_insert.h"
#include "rs_layer.h"

class RS_BlockList;

QList<RS_Entity*> RS_ActionBlocksRemove::insertsOf(const RS_EntityContainer& container, const QString& blockName) {
    // The INSERTs a container holds itself; a block holds its own, and an
    // INSERT, a text or a dimension holds only what it is drawn with.
    QList<RS_Entity*> inserts;
    for (const auto e : container) {
        if (e != nullptr && e->is(RS2::EntityInsert) && !e->isDeleted()
            && static_cast<RS_Insert*>(e)->getName() == blockName) {
            inserts.append(e);
        }
    }
    return inserts;
}

RS_ActionBlocksRemove::RS_ActionBlocksRemove(LC_ActionContext *actionContext)
    :LC_UndoableDocumentModificationAction("Remove Block", actionContext, RS2::ActionBlocksRemove) {}

bool RS_ActionBlocksRemove::doTriggerModifications(LC_DocumentModificationBatch& ctx) {
    if (!((m_graphic != nullptr) && (m_document != nullptr))) { /// fixme - remove, should be checked on init
        finish();
        return false;
    }

    RS_BlockList *blockList = m_graphic->getBlockList();
    QList<RS_Block *> blocks = RS_DIALOGFACTORY->requestSelectedBlocksRemovalDialog(blockList);

    if (blocks.isEmpty()) {
        finish();
        return false;
    }

    // list of containers that might refer to the block via inserts:
    std::vector<RS_EntityContainer *> containerList;
    containerList.push_back(m_graphic);
    for (int bi = 0; bi < blockList->count(); bi++) {
        containerList.push_back(blockList->at(bi));
    }
    // The layouts' entities too, though no view shows them.
    const std::size_t firstLayout = containerList.size();
    for (const auto& paperSpace : m_graphic->paperSpaces().spaces()) {
        containerList.push_back(paperSpace.entities.get());
    }

    bool removing = false;
    for (const auto block: std::as_const(blocks)) {
        if (nullptr == block) {
            continue;
        }
        // Every INSERT of the block. ctx -= only lists an entity for removal,
        // which happens once this returns: one pass per container finds each
        // of them, where repeating the search until none was left never
        // ended. An INSERT is in one container and of one block, so none is
        // listed twice.
        QList<RS_Entity*> inserts;
        int lockedOnLayouts = 0;
        for (std::size_t i = 0; i < containerList.size(); ++i) {
            for (const auto insert : insertsOf(*containerList[i], block->getName())) {
                const RS_Layer* layer = insert->getLayer(true);
                if (i >= firstLayout && layer != nullptr && layer->isLocked()) {
                    ++lockedOnLayouts;
                }
                inserts.append(insert);
            }
        }
        // An INSERT on a locked layer is not deleted, and would be left
        // naming a block that is gone. On a layout no view shows it, to
        // unlock its layer: the block stays.
        if (lockedOnLayouts > 0) {
            RS_DIALOGFACTORY->commandMessage(
                tr("Block \"%1\" is not removed: %n of its insert(s) on the drawing's paper-space "
                   "layouts are on a locked layer.", "", lockedOnLayouts)
                    .arg(block->getName()));
            continue;
        }
        ctx -= inserts;
        removing = true;

        // clear selection and active state
        block->selectedInBlockList(false);
        if (block == blockList->getActive()) {
            blockList->activate(nullptr);
        }

        // close all windows that are editing this block:
        RS_DIALOGFACTORY->closeEditBlockWindow(block);

        // Now remove block from the block list, but do not delete:
        ctx -= block;
    }
    if (!removing) {
        finish();
        return false;
    }

    m_graphic->addBlockNotification();
    m_graphic->updateInserts();
    blockList->activate(nullptr);

    finish(); // fixme - is it needed there?

    return true;
}

void RS_ActionBlocksRemove::init(const int status) {
    RS_ActionInterface::init(status);
    trigger();
}
