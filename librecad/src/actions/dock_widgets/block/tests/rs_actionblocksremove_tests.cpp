/****************************************************************************
**
** This file is part of the LibreCAD project, a 2D CAD program
**
** Copyright (C) 2026 LibreCAD.org
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
**********************************************************************/

// Removing a block removes its INSERTs wherever they are: in model space, in
// another block, and on a layout, which no view shows. The action used to
// look for them until it found none, and since listing an INSERT for removal
// does not delete it yet, it never stopped once there was one.

#include <catch2/catch_test_macros.hpp>

#include <QList>

#include "lc_actiontestsupport.h"
#include "lc_paperspacestore.h"
#include "rs_actionblocksremove.h"
#include "rs_block.h"
#include "rs_dialogfactory.h"
#include "rs_dialogfactoryinterface.h"
#include "rs_insert.h"
#include "rs_line.h"

namespace {

/// Answers the removal dialog with the blocks given, as a user would.
class RemovalDialogs final : public RS_DialogFactoryInterface {
public:
    explicit RemovalDialogs(QList<RS_Block*> answer) : m_answer(std::move(answer)) {
        RS_DialogFactory::instance()->setFactoryObject(this);
    }
    ~RemovalDialogs() override { RS_DialogFactory::instance()->setFactoryObject(nullptr); }
    RemovalDialogs(const RemovalDialogs&) = delete;
    RemovalDialogs& operator=(const RemovalDialogs&) = delete;

    QList<RS_Block*> requestSelectedBlocksRemovalDialog(RS_BlockList*) override {
        ++m_asked;
        return m_answer;
    }

    int asked() const { return m_asked; }

private:
    QList<RS_Block*> m_answer;
    int m_asked = 0;
};

RS_Block* addBlock(RS_Graphic& graphic, const QString& name) {
    auto* block = new RS_Block(&graphic, RS_BlockData(name, RS_Vector(0., 0.), false));
    block->addEntity(new RS_Line(block, RS_LineData(RS_Vector(0., 0.), RS_Vector(1., 1.))));
    graphic.addBlock(block, false);
    return block;
}

RS_Insert* addInsert(RS_EntityContainer& container, const QString& name, const double x) {
    auto* insert = new RS_Insert(&container, RS_InsertData(name, RS_Vector(x, 0.), RS_Vector(1., 1.), 0., 1, 1,
                                                           RS_Vector(0., 0.), nullptr, RS2::NoUpdate));
    container.addEntity(insert);
    return insert;
}

} // namespace

TEST_CASE("Removing a block removes its INSERTs in model space, in blocks and on layouts",
          "[actions][blocks][paper-space]") {
    lc::test::ActionFixture<RS_ActionBlocksRemove> f;
    RS_Graphic& graphic = f.m_graphic;

    RS_Block* removed = addBlock(graphic, QStringLiteral("FRAME"));
    RS_Block* holder = addBlock(graphic, QStringLiteral("SHEET"));
    RS_Insert* inModel = addInsert(graphic, QStringLiteral("FRAME"), 0.);
    RS_Insert* secondInModel = addInsert(graphic, QStringLiteral("FRAME"), 10.);
    RS_Insert* otherInModel = addInsert(graphic, QStringLiteral("SHEET"), 20.);
    RS_Insert* inBlock = addInsert(*holder, QStringLiteral("FRAME"), 30.);
    RS_EntityContainer* layout = graphic.paperSpaces().container(QStringLiteral("*Paper_Space0"));
    REQUIRE(layout != nullptr);
    RS_Insert* onLayout = addInsert(*layout, QStringLiteral("FRAME"), 40.);
    RS_EntityContainer* activeLayout = graphic.paperSpaces().container(QStringLiteral("*Paper_Space"));
    RS_Insert* onActiveLayout = addInsert(*activeLayout, QStringLiteral("FRAME"), 50.);

    RemovalDialogs dialogs({removed});
    f.m_action->init(0);

    CHECK(dialogs.asked() == 1);
    CHECK(f.m_action->isFinished());
    for (const RS_Insert* insert : {inModel, secondInModel, inBlock, onLayout, onActiveLayout}) {
        CHECK(insert->isDeleted());
    }
    CHECK_FALSE(otherInModel->isDeleted());
    CHECK(removed->isDeleted());
    CHECK_FALSE(holder->isDeleted());

    // One undo brings all of it back.
    REQUIRE(graphic.undo());
    for (const RS_Insert* insert : {inModel, secondInModel, inBlock, onLayout, onActiveLayout}) {
        CHECK_FALSE(insert->isDeleted());
    }
    CHECK_FALSE(removed->isDeleted());
}

TEST_CASE("The INSERTs of a block are found in one pass", "[actions][blocks]") {
    lc::test::ActionFixture<RS_ActionBlocksRemove> f;
    RS_Graphic& graphic = f.m_graphic;
    addBlock(graphic, QStringLiteral("A"));
    RS_Insert* first = addInsert(graphic, QStringLiteral("A"), 0.);
    addInsert(graphic, QStringLiteral("B"), 1.);
    RS_Insert* deleted = addInsert(graphic, QStringLiteral("A"), 2.);
    deleted->setFlag(RS2::FlagDeleted);
    RS_Insert* last = addInsert(graphic, QStringLiteral("A"), 3.);

    const QList<RS_Entity*> found = RS_ActionBlocksRemove::insertsOf(graphic, QStringLiteral("A"));
    REQUIRE(found.size() == 2);
    CHECK(found.front() == first);
    CHECK(found.back() == last);
}
