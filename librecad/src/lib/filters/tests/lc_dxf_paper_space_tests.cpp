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

// LibreCAD shows and edits model space only. What a drawing keeps on its
// layouts used to be drawn across model space; it is now kept apart
// (RS_Graphic::paperSpaces()), out of model space and of the block list, and
// a DXF save writes it back: the active layout's entities to ENTITIES with
// group 67, another layout's to its *Paper_SpaceN block. A drawing made on a
// layout alone still shows it in model space, as it always did.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <QString>
#include <QStringList>

#include "lc_documentsstorage.h"
#include "lc_filtertestsupport.h"
#include "lc_paperspacestore.h"
#include "rs_block.h"
#include "rs_dialogfactory.h"
#include "rs_dialogfactoryinterface.h"
#include "rs_polyline.h"
#include "rs_entitycontainer.h"
#include "rs_filterdxfrw.h"
#include "rs_graphic.h"
#include "rs_insert.h"
#include "rs_layer.h"

namespace {

using lc::test::DxfRecord;

// A DXF R2000 with one LINE and one INSERT in model space, three entities on
// the active layout (Layout1: ENTITIES, group 67) and two on another
// (Layout2: the *Paper_Space0 block). "XPaper_Space_Frame" is an ordinary
// block, whatever its name looks like. Its dictionaries are where AutoCAD
// keeps them -- the root at C, ACAD_GROUP at D, ACAD_LAYOUT elsewhere -- or,
// with @p ezdxfDictionaries, where ezdxf does: the root at A, ACAD_GROUP at C
// and ACAD_LAYOUT at D, the handles the writer gives its own two.
std::string layoutsDxf(const bool withModelSpace = true, const bool ezdxfDictionaries = false,
                       const bool withOtherLayout = true) {
    std::vector<std::string> lines;
    const auto add = [&lines](std::initializer_list<std::pair<int, std::string>> groups) {
        for (const auto& [code, value] : groups) {
            lines.push_back(std::to_string(code));
            lines.push_back(value);
        }
    };
    add({{0, "SECTION"}, {2, "HEADER"}, {9, "$ACADVER"}, {1, "AC1015"},
         {9, "$HANDSEED"}, {5, "FFFF"}, {0, "ENDSEC"}});
    add({{0, "SECTION"}, {2, "TABLES"}, {0, "TABLE"}, {2, "BLOCK_RECORD"}, {5, "1"},
         {330, "0"}, {100, "AcDbSymbolTable"}, {70, "4"}});
    for (const auto& [handle, name, layout] :
         {std::tuple<std::string, std::string, std::string>{"1F", "*Model_Space", "22"},
          {"1E", "*Paper_Space", "26"},
          {"40", "*Paper_Space0", "41"},
          {"60", "XPaper_Space_Frame", "0"}}) {
        add({{0, "BLOCK_RECORD"}, {5, handle}, {330, "1"}, {100, "AcDbSymbolTableRecord"},
             {100, "AcDbBlockTableRecord"}, {2, name}});
        if (layout != "0")
            add({{340, layout}});
    }
    add({{0, "ENDTAB"}, {0, "ENDSEC"}, {0, "SECTION"}, {2, "BLOCKS"}});
    const auto line = [&add](const std::string& handle, const std::string& owner, const bool paper,
                             const std::string& x2, const std::string& y2) {
        add({{0, "LINE"}, {5, handle}, {330, owner}, {100, "AcDbEntity"}});
        if (paper)
            add({{67, "1"}});
        add({{8, "0"}, {100, "AcDbLine"}, {10, "0.0"}, {20, "0.0"}, {30, "0.0"},
             {11, x2}, {21, y2}, {31, "0.0"}});
    };
    const auto circle = [&add](const std::string& handle, const std::string& owner,
                               const std::string& radius) {
        add({{0, "CIRCLE"}, {5, handle}, {330, owner}, {100, "AcDbEntity"}, {67, "1"}, {8, "0"},
             {100, "AcDbCircle"}, {10, "210.0"}, {20, "148.0"}, {30, "0.0"}, {40, radius}});
    };
    for (const auto& [begin, end, owner, name, paper] :
         {std::tuple<std::string, std::string, std::string, std::string, bool>{
              "20", "21", "1F", "*Model_Space", false},
          {"1C", "1D", "1E", "*Paper_Space", true},
          {"42", "43", "40", "*Paper_Space0", true},
          {"62", "63", "60", "XPaper_Space_Frame", false}}) {
        add({{0, "BLOCK"}, {5, begin}, {330, owner}, {100, "AcDbEntity"}});
        if (paper)
            add({{67, "1"}});
        add({{8, "0"}, {100, "AcDbBlockBegin"}, {2, name}, {70, "0"}, {10, "0.0"}, {20, "0.0"},
             {30, "0.0"}, {3, name}, {1, ""}});
        if (name == "*Paper_Space0" && withOtherLayout) {
            line("44", "40", true, "420.0", "297.0");
            circle("45", "40", "50.0");
        } else if (name == "XPaper_Space_Frame") {
            add({{0, "LINE"}, {5, "64"}, {330, "60"}, {100, "AcDbEntity"}, {8, "0"},
                 {100, "AcDbLine"}, {10, "0.0"}, {20, "0.0"}, {30, "0.0"}, {11, "5.0"},
                 {21, "5.0"}, {31, "0.0"}});
        }
        add({{0, "ENDBLK"}, {5, end}, {330, owner}, {100, "AcDbEntity"}});
        if (paper)
            add({{67, "1"}});
        add({{8, "0"}, {100, "AcDbBlockEnd"}});
    }
    add({{0, "ENDSEC"}, {0, "SECTION"}, {2, "ENTITIES"}});
    if (withModelSpace) {
        line("50", "1F", false, "100.0", "50.0");
        add({{0, "INSERT"}, {5, "54"}, {330, "1F"}, {100, "AcDbEntity"}, {8, "0"},
             {100, "AcDbBlockReference"}, {2, "XPaper_Space_Frame"}, {10, "10.0"},
             {20, "10.0"}, {30, "0.0"}});
    }
    line("51", "1E", true, "841.0", "594.0");
    circle("52", "1E", "25.0");
    add({{0, "TEXT"}, {5, "53"}, {330, "1E"}, {100, "AcDbEntity"}, {67, "1"}, {8, "0"},
         {100, "AcDbText"}, {10, "10.0"}, {20, "10.0"}, {30, "0.0"}, {40, "5.0"},
         {1, "SHEET"}, {100, "AcDbText"}});
    const std::string layoutDictionary = ezdxfDictionaries ? "D" : "1A";
    if (ezdxfDictionaries) {
        add({{0, "ENDSEC"}, {0, "SECTION"}, {2, "OBJECTS"}, {0, "DICTIONARY"}, {5, "A"},
             {330, "0"}, {100, "AcDbDictionary"}, {281, "1"}, {3, "ACAD_GROUP"}, {350, "C"},
             {3, "ACAD_LAYOUT"}, {350, "D"},
             {0, "DICTIONARY"}, {5, "C"}, {330, "A"}, {100, "AcDbDictionary"}, {281, "1"},
             {0, "DICTIONARY"}, {5, "D"}, {330, "A"}, {100, "AcDbDictionary"}, {281, "1"},
             {3, "Layout1"}, {350, "26"}, {3, "Layout2"}, {350, "41"}, {3, "Model"}, {350, "22"}});
    } else {
        add({{0, "ENDSEC"}, {0, "SECTION"}, {2, "OBJECTS"}, {0, "DICTIONARY"}, {5, "C"},
             {330, "0"}, {100, "AcDbDictionary"}, {281, "1"}, {3, "ACAD_LAYOUT"}, {350, "1A"},
             {0, "DICTIONARY"}, {5, "1A"}, {330, "C"}, {100, "AcDbDictionary"}, {281, "1"},
             {3, "Layout1"}, {350, "26"}, {3, "Layout2"}, {350, "41"}, {3, "Model"}, {350, "22"}});
    }
    for (const auto& [handle, name, tab, record] :
         {std::tuple<std::string, std::string, std::string, std::string>{"22", "Model", "0", "1F"},
          {"26", "Layout1", "1", "1E"},
          {"41", "Layout2", "2", "40"}}) {
        add({{0, "LAYOUT"}, {5, handle}, {330, layoutDictionary}, {100, "AcDbPlotSettings"}, {1, ""},
             {2, ""}, {4, ""}, {6, ""}, {40, "0.0"}, {41, "0.0"}, {42, "0.0"}, {43, "0.0"},
             {44, "0.0"}, {45, "0.0"}, {46, "0.0"}, {47, "0.0"}, {48, "0.0"}, {49, "0.0"},
             {140, "0.0"}, {141, "0.0"}, {142, "1.0"}, {143, "1.0"}, {70, "0"}, {72, "0"},
             {73, "0"}, {74, "0"}, {7, ""}, {75, "0"}, {147, "1.0"}, {148, "0.0"},
             {149, "0.0"}, {100, "AcDbLayout"}, {1, name}, {70, name == "Model" ? "1" : "0"},
             {71, tab}, {10, "0.0"}, {20, "0.0"}, {11, "420.0"}, {21, "297.0"}, {12, "0.0"},
             {22, "0.0"}, {32, "0.0"}, {14, "0.0"}, {24, "0.0"}, {34, "0.0"}, {15, "0.0"},
             {25, "0.0"}, {35, "0.0"}, {146, "0.0"}, {13, "0.0"}, {23, "0.0"}, {33, "0.0"},
             {16, "1.0"}, {26, "0.0"}, {36, "0.0"}, {17, "0.0"}, {27, "1.0"}, {37, "0.0"},
             {76, "0"}, {330, record}});
    }
    add({{0, "ENDSEC"}, {0, "EOF"}});
    std::string text;
    for (const std::string& value : lines)
        text += value + "\n";
    return text;
}

void writeText(const std::filesystem::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary);
    out << text;
}

unsigned liveCount(const RS_EntityContainer& container) {
    unsigned count = 0;
    for (const RS_Entity* entity : container) {
        if (entity != nullptr && !entity->getFlag(RS2::FlagDeleted))
            ++count;
    }
    return count;
}

unsigned paperSpaceCount(const RS_Graphic& graphic, const QString& name) {
    const LC_PaperSpaceStore::Space* space = graphic.paperSpaces().find(name);
    return space == nullptr ? 0u : liveCount(*space->entities);
}

// Where a written DXF put its entities: "model" and "paper" in ENTITIES (by
// group 67), "block <name>" inside a block definition, with how many of
// those carry group 67 = 1 under "67 <name>".
std::map<std::string, int> writtenSpaces(const std::filesystem::path& file) {
    std::map<std::string, int> spaces;
    std::string section;
    std::string block;
    for (const DxfRecord& record : lc::test::dxfRecords(file)) {
        const std::string& type = record.type();
        if (type == "SECTION") {
            const auto names = record.own(2);
            section = names.empty() ? std::string() : names.front();
            continue;
        }
        if (type == "ENDSEC") {
            section.clear();
            continue;
        }
        if (section == "BLOCKS") {
            if (type == "BLOCK") {
                const auto names = record.own(2);
                block = names.empty() ? std::string() : names.front();
                continue;
            }
            if (type == "ENDBLK") {
                block.clear();
                continue;
            }
        }
        if (type == "VERTEX" || type == "SEQEND" || type == "ATTRIB")
            continue;
        const auto space = record.own(67);
        const bool paper = !space.empty() && space.front() == "1";
        if (section == "ENTITIES") {
            ++spaces[paper ? "paper" : "model"];
        } else if (section == "BLOCKS" && !block.empty()) {
            ++spaces["block " + block];
            if (paper)
                ++spaces["67 " + block];
        }
    }
    return spaces;
}

} // namespace

TEST_CASE("Paper-space block record names are matched exactly", "[dxf][paper-space]") {
    for (const char* name : {"*Paper_Space", "*PAPER_SPACE", "*Paper_Space0", "*paper_space12",
                             "$PAPER_SPACE"}) {
        CAPTURE(name);
        CHECK(LC_PaperSpaceStore::isPaperSpaceName(QString::fromLatin1(name)));
        CHECK_FALSE(LC_PaperSpaceStore::isModelSpaceName(QString::fromLatin1(name)));
    }
    for (const char* name : {"*Model_Space", "*MODEL_SPACE", "$MODEL_SPACE"}) {
        CAPTURE(name);
        CHECK(LC_PaperSpaceStore::isModelSpaceName(QString::fromLatin1(name)));
        CHECK_FALSE(LC_PaperSpaceStore::isPaperSpaceName(QString::fromLatin1(name)));
    }
    for (const char* name : {"XPaper_Space_Frame", "*Paper_Space_Frame", "*Paper_SpaceX",
                             "Paper_Space", "*Model_Space1", "XModel_Space"}) {
        CAPTURE(name);
        CHECK_FALSE(LC_PaperSpaceStore::isPaperSpaceName(QString::fromLatin1(name)));
        CHECK_FALSE(LC_PaperSpaceStore::isModelSpaceName(QString::fromLatin1(name)));
    }
    CHECK(LC_PaperSpaceStore::isActivePaperSpaceName(QStringLiteral("$PAPER_SPACE")));
    CHECK_FALSE(LC_PaperSpaceStore::isActivePaperSpaceName(QStringLiteral("*Paper_Space0")));
}

TEST_CASE("Layout entities stay out of model space and a DXF save writes them back",
          "[dxf][paper-space]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_layouts");
    const auto source = directory.file("layouts_r2000.dxf");
    writeText(source, layoutsDxf());

    RS_Graphic graphic;
    REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));

    // Model space holds its own two entities, and nothing of the layouts.
    CHECK(liveCount(graphic) == 2);
    // The block named like a paper space is a block; the paper spaces are not.
    RS_Block* frame = graphic.findBlock(QStringLiteral("XPaper_Space_Frame"));
    REQUIRE(frame != nullptr);
    CHECK(liveCount(*frame) == 1);
    CHECK(graphic.findBlock(QStringLiteral("*Paper_Space0")) == nullptr);
    CHECK(graphic.findBlock(QStringLiteral("*Paper_Space")) == nullptr);
    CHECK(graphic.countBlocks() == 1);
    // The layouts' entities are kept, each with its layout.
    CHECK(graphic.paperSpaces().count() == 5);
    CHECK(paperSpaceCount(graphic, QStringLiteral("*Paper_Space")) == 3);
    CHECK(paperSpaceCount(graphic, QStringLiteral("*Paper_Space0")) == 2);

    const auto [format, name] = GENERATE(table<RS2::FormatType, const char*>(
        {{RS2::FormatDXFRW2000, "r2000"}, {RS2::FormatDXFRW, "r2007"},
         {RS2::FormatDXFRW2018, "r2018"}}));
    CAPTURE(name);
    const auto saved = directory.file(std::string("saved_") + name + ".dxf");
    RS_FilterDXFRW writer;
    REQUIRE(writer.fileExport(graphic, lc::test::toQString(saved), format));
    // Whatever else this version cannot hold (an object of the R2000 source),
    // it holds every layout entity.
    INFO(writer.exportReport().toStdString());
    CHECK_FALSE(writer.exportReport().contains(QStringLiteral("paper-space")));

    const auto spaces = writtenSpaces(saved);
    const auto at = [&spaces](const std::string& key) {
        const auto it = spaces.find(key);
        return it == spaces.end() ? 0 : it->second;
    };
    CHECK(at("model") == 2);
    CHECK(at("paper") == 3);
    CHECK(at("block *Paper_Space0") == 2);
    CHECK(at("67 *Paper_Space0") == 2);
    CHECK(at("block *Paper_Space") == 0);
    CHECK(at("block XPaper_Space_Frame") == 1);

    // Opened again, the drawing is the same.
    RS_Graphic reopened;
    REQUIRE(lc::test::importDrawing(reopened, saved, RS2::FormatDXFRW));
    CHECK(liveCount(reopened) == 2);
    CHECK(paperSpaceCount(reopened, QStringLiteral("*Paper_Space")) == 3);
    CHECK(paperSpaceCount(reopened, QStringLiteral("*Paper_Space0")) == 2);
}

TEST_CASE("An R12 DXF holds the active layout only, and the save says what it left out",
          "[dxf][paper-space]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_r12");
    const auto source = directory.file("layouts_r2000.dxf");
    writeText(source, layoutsDxf());
    RS_Graphic graphic;
    REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));

    const auto saved = directory.file("saved_r12.dxf");
    RS_FilterDXFRW writer;
    REQUIRE(writer.fileExport(graphic, lc::test::toQString(saved), RS2::FormatDXFRW12));
    CHECK(writer.exportReport().contains(
        QStringLiteral("2 object(s) on paper-space layouts")));
    const auto spaces = writtenSpaces(saved);
    CHECK(spaces.count("paper") == 1);
    CHECK(spaces.at("paper") == 3);
    CHECK(spaces.at("model") == 2);

    RS_Graphic reopened;
    REQUIRE(lc::test::importDrawing(reopened, saved, RS2::FormatDXFRW));
    CHECK(liveCount(reopened) == 2);
    CHECK(paperSpaceCount(reopened, QStringLiteral("*Paper_Space")) == 3);
}

TEST_CASE("A drawing made on its layouts alone shows the active one in model space",
          "[dxf][paper-space]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_only");
    const auto source = directory.file("layouts_only_r2000.dxf");
    writeText(source, layoutsDxf(false));

    RS_Graphic graphic;
    REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));
    // The active layout's three entities are in model space, as before; the
    // other layout stays on its own.
    CHECK(liveCount(graphic) == 3);
    CHECK(paperSpaceCount(graphic, QStringLiteral("*Paper_Space")) == 0);
    CHECK(paperSpaceCount(graphic, QStringLiteral("*Paper_Space0")) == 2);
}

TEST_CASE("Layout entities follow a renamed block and keep their layer",
          "[dxf][paper-space]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_edits");
    const auto source = directory.file("layouts_r2000.dxf");
    std::string text = layoutsDxf();
    // Put the INSERT on the active layout instead, on a layer of its own.
    const std::string modelInsert = "0\nINSERT\n5\n54\n330\n1F\n100\nAcDbEntity\n8\n0\n";
    const auto at = text.find(modelInsert);
    REQUIRE(at != std::string::npos);
    text.replace(at, modelInsert.size(),
                 "0\nINSERT\n5\n54\n330\n1E\n100\nAcDbEntity\n67\n1\n8\nFRAME\n");
    writeText(source, text);

    RS_Graphic graphic;
    REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));
    REQUIRE(paperSpaceCount(graphic, QStringLiteral("*Paper_Space")) == 4);

    graphic.renameInserts(QStringLiteral("XPaper_Space_Frame"), QStringLiteral("Frame"));
    // A layer a layout's entity is on stays: nothing shows the layout, and
    // moving its entities to layer 0 could not be undone.
    RS_Layer* layer = graphic.findLayer(QStringLiteral("FRAME"));
    REQUIRE(layer != nullptr);
    CHECK(graphic.paperSpaces().countOnLayer(QStringLiteral("FRAME")) == 1);
    CHECK_FALSE(graphic.removeLayer(layer));
    CHECK(graphic.findLayer(QStringLiteral("FRAME")) == layer);

    const LC_PaperSpaceStore::Space* active =
        graphic.paperSpaces().find(QStringLiteral("*Paper_Space"));
    REQUIRE(active != nullptr);
    const RS_Insert* insert = nullptr;
    for (const RS_Entity* entity : *active->entities) {
        if (entity->rtti() == RS2::EntityInsert)
            insert = static_cast<const RS_Insert*>(entity);
    }
    REQUIRE(insert != nullptr);
    CHECK(insert->getName() == QStringLiteral("Frame"));
    REQUIRE(insert->getLayer() != nullptr);
    CHECK(insert->getLayer()->getName() == QStringLiteral("FRAME"));

    // A layer only model space uses goes, as it always did.
    graphic.addLayer(new RS_Layer(QStringLiteral("SCRATCH")));
    RS_Layer* scratch = graphic.findLayer(QStringLiteral("SCRATCH"));
    REQUIRE(scratch != nullptr);
    CHECK(graphic.removeLayer(scratch));
    CHECK(graphic.findLayer(QStringLiteral("SCRATCH")) == nullptr);

    // Model space, the blocks and the layouts: 1 + 1 + 4 + 2 entities.
    int visited = 0;
    graphic.visitDrawingEntities([&visited](RS_Entity*) { ++visited; });
    CHECK(visited == 8);
}

namespace {
// What a reader needs to find a layout and its paper space in a DXF: the
// root dictionary's ACAD_LAYOUT entry names a dictionary that lists the
// LAYOUT, which is owned by that dictionary and names its BLOCK_RECORD.
// Answers, for each layout name, the name of the block record it reaches, or
// "orphan" when the chain breaks.
std::map<std::string, std::string> reachableLayouts(const std::filesystem::path& file) {
    std::map<std::string, DxfRecord> byHandle;
    std::vector<DxfRecord> dictionaries;
    std::vector<DxfRecord> layouts;
    for (const DxfRecord& record : lc::test::dxfRecords(file)) {
        const auto handles = record.own(5);
        if (record.type() == "SECTION" || handles.empty())
            continue;
        byHandle.emplace(handles.front(), record);
        if (record.type() == "DICTIONARY")
            dictionaries.push_back(record);
        else if (record.type() == "LAYOUT")
            layouts.push_back(record);
    }
    // The entries of a dictionary, name -> handle.
    const auto entries = [](const DxfRecord& dictionary) {
        std::map<std::string, std::string> found;
        const auto& groups = dictionary.m_groups;
        for (std::size_t i = 0; i + 1 < groups.size(); ++i) {
            if (groups[i].m_code == 3 && (groups[i + 1].m_code == 350 || groups[i + 1].m_code == 360))
                found.emplace(groups[i].m_value, groups[i + 1].m_value);
        }
        return found;
    };
    std::string layoutDictionary;
    for (const DxfRecord& dictionary : dictionaries) {
        const auto owners = dictionary.own(330);
        if (!owners.empty() && owners.front() == "0") {
            const auto root = entries(dictionary);
            const auto it = root.find("ACAD_LAYOUT");
            if (it != root.end())
                layoutDictionary = it->second;
        }
    }
    std::map<std::string, std::string> reached;
    for (const DxfRecord& layout : layouts) {
        const auto names = layout.own(1);
        const auto pointers = layout.own(330);
        if (names.empty() || pointers.size() < 2)
            continue;
        const std::string& name = names.back();
        std::string blockRecord = "orphan";
        const auto dictionary = byHandle.find(pointers.front());
        if (pointers.front() == layoutDictionary && dictionary != byHandle.end() &&
            dictionary->second.type() == "DICTIONARY") {
            const auto listed = entries(dictionary->second);
            const auto entry = listed.find(name);
            const auto record = byHandle.find(pointers.back());
            if (entry != listed.end() && entry->second == layout.own(5).front() &&
                record != byHandle.end() && record->second.type() == "BLOCK_RECORD" &&
                !record->second.own(2).empty())
                blockRecord = record->second.own(2).front();
        }
        reached[name] = blockRecord;
    }
    return reached;
}
} // namespace

TEST_CASE("A layout stays reachable when the source keeps a dictionary at C or D",
          "[dxf][paper-space]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_ezdxf_handles");
    // ezdxf's handles: ACAD_GROUP at C, ACAD_LAYOUT at D. The writer's own
    // root and ACAD_GROUP take C and D; ACAD_LAYOUT used to be dropped with
    // them, which left Layout2's *Paper_Space0 block an orphan that a reader
    // deletes with its content.
    const bool ezdxf = GENERATE(true, false);
    CAPTURE(ezdxf);
    const auto source = directory.file(ezdxf ? "ezdxf.dxf" : "autocad.dxf");
    writeText(source, layoutsDxf(true, ezdxf));
    RS_Graphic graphic;
    REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));
    REQUIRE(paperSpaceCount(graphic, QStringLiteral("*Paper_Space0")) == 2);

    const auto [format, name] = GENERATE(table<RS2::FormatType, const char*>(
        {{RS2::FormatDXFRW2000, "r2000"}, {RS2::FormatDXFRW, "r2007"},
         {RS2::FormatDXFRW2018, "r2018"}}));
    CAPTURE(name);
    const auto saved = directory.file(std::string(ezdxf ? "ezdxf_" : "autocad_") + name + ".dxf");
    RS_FilterDXFRW writer;
    REQUIRE(writer.fileExport(graphic, lc::test::toQString(saved), format));
    INFO(writer.exportReport().toStdString());
    CHECK_FALSE(writer.exportReport().contains(QStringLiteral("paper-space")));
    CHECK(lc::test::duplicateDxfHandles(saved).empty());

    const auto reached = reachableLayouts(saved);
    REQUIRE(reached.count("Layout1") == 1);
    REQUIRE(reached.count("Layout2") == 1);
    CHECK(reached.at("Layout1") == "*Paper_Space");
    CHECK(reached.at("Layout2") == "*Paper_Space0");
    CHECK(writtenSpaces(saved).at("block *Paper_Space0") == 2);

    RS_Graphic reopened;
    REQUIRE(lc::test::importDrawing(reopened, saved, RS2::FormatDXFRW));
    CHECK(paperSpaceCount(reopened, QStringLiteral("*Paper_Space")) == 3);
    CHECK(paperSpaceCount(reopened, QStringLiteral("*Paper_Space0")) == 2);
}

TEST_CASE("A DWG save writes every layout's entities back to its layout",
          "[dxf][dwg][paper-space]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_dwg");
    const auto source = directory.file("layouts_r2000.dxf");
    writeText(source, layoutsDxf());
    RS_Graphic graphic;
    REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));

    const auto [format, name] = GENERATE(table<RS2::FormatType, const char*>(
        {{RS2::FormatDWG, "r2000"}, {RS2::FormatDWG2018, "r2018"}}));
    CAPTURE(name);
    // Nothing a DWG of this version keeps is left out, so a save in place
    // may go ahead.
    RS_FilterDXFRW writer;
    CHECK(writer.exportLeavesOut(graphic, format).isEmpty());
    const auto saved = directory.file(std::string("saved_") + name + ".dwg");
    REQUIRE(writer.fileExport(graphic, lc::test::toQString(saved), format));
    INFO(writer.exportReport().toStdString());
    CHECK_FALSE(writer.exportReport().contains(QStringLiteral("paper-space")));

    RS_Graphic reopened;
    REQUIRE(lc::test::importDrawing(reopened, saved, RS2::FormatDWG));
    CHECK(liveCount(reopened) == 2);
    CHECK(paperSpaceCount(reopened, QStringLiteral("*Paper_Space")) == 3);
    CHECK(paperSpaceCount(reopened, QStringLiteral("*Paper_Space0")) == 2);
    CHECK(reopened.findBlock(QStringLiteral("*Paper_Space0")) == nullptr);
    // The LAYOUT of Layout2 names the block that holds its entities.
    const auto& metadata = reopened.dwgAdvancedMetadata();
    bool layout2Found = false;
    for (const auto& layout : metadata.layouts()) {
        if (layout.name != "Layout2")
            continue;
        layout2Found = true;
        const auto record = metadata.spaceBlockRecords().find(layout.paperSpaceBlockRecordHandle);
        REQUIRE(record != metadata.spaceBlockRecords().end());
        CHECK(QString::fromStdString(record->second).compare(QStringLiteral("*Paper_Space0"),
                                                             Qt::CaseInsensitive) == 0);
    }
    CHECK(layout2Found);

    // Saved again as DXF, the layouts stay reachable. The DWG's root lists
    // ACAD_GROUP alone; the dictionary of the layouts gets its name back.
    const auto back = directory.file(std::string("back_") + name + ".dxf");
    RS_FilterDXFRW dxfWriter;
    REQUIRE(dxfWriter.fileExport(reopened, lc::test::toQString(back), RS2::FormatDXFRW2018));
    INFO(dxfWriter.exportReport().toStdString());
    CHECK_FALSE(dxfWriter.exportReport().contains(QStringLiteral("paper-space")));
    const auto reached = reachableLayouts(back);
    REQUIRE(reached.count("Layout1") == 1);
    REQUIRE(reached.count("Layout2") == 1);
    CHECK(reached.at("Layout1") == "*Paper_Space");
    CHECK(reached.at("Layout2") == "*Paper_Space0");
    CHECK(writtenSpaces(back).at("block *Paper_Space0") == 2);
}

TEST_CASE("What a save would leave out of the layouts is known before it",
          "[dxf][paper-space]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_leaves_out");
    const auto source = directory.file("layouts_r2000.dxf");
    writeText(source, layoutsDxf());
    RS_Graphic graphic;
    REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));

    RS_FilterDXFRW filter;
    for (const RS2::FormatType format : {RS2::FormatDXFRW12, RS2::FormatDXFRW14}) {
        CAPTURE(static_cast<int>(format));
        CHECK(filter.exportLeavesOut(graphic, format) ==
              QStringLiteral("2 object(s) on paper-space layouts (Layout2)"));
    }
    for (const RS2::FormatType format : {RS2::FormatDXFRW2000, RS2::FormatDXFRW, RS2::FormatDXFRW2018,
                                         RS2::FormatDWG, RS2::FormatDWG2018}) {
        CAPTURE(static_cast<int>(format));
        CHECK(filter.exportLeavesOut(graphic, format).isEmpty());
    }
}

namespace {
/// Keeps what the import tells the user.
class CommandLine final : public RS_DialogFactoryInterface {
public:
    CommandLine() { RS_DialogFactory::instance()->setFactoryObject(this); }
    ~CommandLine() override { RS_DialogFactory::instance()->setFactoryObject(nullptr); }
    CommandLine(const CommandLine&) = delete;
    CommandLine& operator=(const CommandLine&) = delete;
    void commandMessage(const QString& message) override {
        if (message.contains(QStringLiteral("paper-space layouts")))
            messages << message;
    }
    QStringList messages;
};
} // namespace

TEST_CASE("Only a drawing opened as a document tells what its layouts keep",
          "[dxf][paper-space]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_notices");
    const auto source = directory.file("layouts_r2000.dxf");
    writeText(source, layoutsDxf());

    SECTION("a thumbnail, a template or a preview says nothing") {
        CommandLine commandLine;
        RS_Graphic graphic;
        REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));
        CHECK(commandLine.messages.isEmpty());
        // Read silently, the layouts' content is still kept.
        CHECK(graphic.paperSpaces().count() == 5);
    }
    SECTION("a document says what is kept, and which saves keep it") {
        CommandLine commandLine;
        RS_Graphic graphic;
        graphic.setReportsImportNotices(true);
        REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));
        REQUIRE(commandLine.messages.size() == 1);
        const QString& message = commandLine.messages.front();
        CHECK(message.startsWith(QStringLiteral("5 object(s) on paper-space layouts (Layout1, Layout2)")));
        CHECK(message.contains(QStringLiteral("as DWG or as DXF R2000 or later")));
        CHECK(message.contains(QStringLiteral("R12 or R14")));
    }
    SECTION("content on the active layout alone is kept by every save") {
        // Layout2 holds nothing: its *Paper_Space0 block is empty.
        const auto activeOnly = directory.file("active_only.dxf");
        writeText(activeOnly, layoutsDxf(true, false, false));
        CommandLine commandLine;
        RS_Graphic graphic;
        graphic.setReportsImportNotices(true);
        REQUIRE(lc::test::importDrawing(graphic, activeOnly, RS2::FormatDXFRW));
        REQUIRE(paperSpaceCount(graphic, QStringLiteral("*Paper_Space0")) == 0);
        REQUIRE(commandLine.messages.size() == 1);
        CHECK(commandLine.messages.front() ==
              QStringLiteral("3 object(s) on paper-space layouts (Layout1) are not shown in model "
                             "space; they are kept and saved with the drawing."));
    }
}

namespace {
/// Answers Save As by cancelling it, and notes what it offered.
class AskingStorage final : public LC_DocumentsStorage {
public:
    int asked = 0;
    RS2::FormatType offered = RS2::FormatUnknown;

protected:
    LC_FileDialogService::FileDialogResult askSaveFileDetails(const QString&,
                                                              const RS2::FormatType preferredType) override {
        ++asked;
        offered = preferredType;
        return {};
    }
};
} // namespace

TEST_CASE("A save in place that would leave layout content out asks for a name first",
          "[dxf][paper-space][save]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_save_in_place");
    const auto source = directory.file("layouts.dxf");
    writeText(source, layoutsDxf());
    const std::string original = lc::test::fileBytes(source);
    RS_Graphic graphic;
    REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));
    graphic.setFilename(lc::test::toQString(source));
    // As if the file were an R14 DXF, which holds the active layout only:
    // Save may not overwrite it with Layout2 left out.
    graphic.setFormatType(RS2::FormatDXFRW14);
    graphic.setModified(true);

    CommandLine commandLine;
    AskingStorage storage;
    bool cancelled = false;
    CHECK(storage.saveDocument(&graphic, nullptr, cancelled));
    CHECK(cancelled);
    CHECK(storage.asked == 1);
    CHECK(storage.offered == RS2::FormatDXFRW2018);
    CHECK(lc::test::fileBytes(source) == original);
    CHECK(graphic.isModified());
    REQUIRE(commandLine.messages.size() == 1);
    CHECK(commandLine.messages.front().contains(
        QStringLiteral("would leave out 2 object(s) on paper-space layouts (Layout2)")));
}

TEST_CASE("A layout shown in model space keeps its drawing order",
          "[dxf][paper-space]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_order");
    // Two hatches between lines on the active layout of a drawing without
    // model space content, which is then shown in model space; the same
    // content read as model space is the reference.
    std::string paperOnly = layoutsDxf(false);
    const auto hatch = [](const std::string& handle, const bool paper) {
        std::string text = "0\nHATCH\n5\n" + handle + "\n330\n" + (paper ? "1E" : "1F") +
                           "\n100\nAcDbEntity\n";
        if (paper)
            text += "67\n1\n";
        text += "8\n0\n100\nAcDbHatch\n10\n0.0\n20\n0.0\n30\n0.0\n210\n0.0\n220\n0.0\n230\n1.0\n"
                "2\nSOLID\n70\n1\n71\n0\n91\n1\n92\n2\n72\n0\n73\n1\n93\n4\n"
                "10\n0.0\n20\n0.0\n10\n10.0\n20\n0.0\n10\n10.0\n20\n10.0\n10\n0.0\n20\n10.0\n"
                "97\n0\n75\n0\n76\n1\n98\n0\n";
        return text;
    };
    const std::string anchor = "0\nTEXT\n5\n53\n";
    const std::size_t at = paperOnly.find(anchor);
    REQUIRE(at != std::string::npos);
    std::string modelOnly = paperOnly;
    paperOnly.insert(at, hatch("70", true) + hatch("71", true));
    modelOnly.insert(at, hatch("70", false) + hatch("71", false));
    // The model-space reference: every entity of ENTITIES in model space.
    for (const std::string& paperLine : {std::string("330\n1E\n100\nAcDbEntity\n67\n1\n")}) {
        std::size_t from = modelOnly.find("2\nENTITIES\n");
        REQUIRE(from != std::string::npos);
        for (std::size_t pos = modelOnly.find(paperLine, from); pos != std::string::npos;
             pos = modelOnly.find(paperLine, pos)) {
            modelOnly.replace(pos, paperLine.size(), "330\n1F\n100\nAcDbEntity\n");
        }
    }
    const auto paperFile = directory.file("paper_only.dxf");
    const auto modelFile = directory.file("model_only.dxf");
    writeText(paperFile, paperOnly);
    writeText(modelFile, modelOnly);

    const auto order = [](const RS_EntityContainer& container) {
        std::vector<std::uint32_t> handles;
        for (const RS_Entity* entity : container) {
            if (entity != nullptr && !entity->getFlag(RS2::FlagDeleted))
                handles.push_back(entity->sourceHandle());
        }
        return handles;
    };
    RS_Graphic shown;
    REQUIRE(lc::test::importDrawing(shown, paperFile, RS2::FormatDXFRW));
    RS_Graphic reference;
    REQUIRE(lc::test::importDrawing(reference, modelFile, RS2::FormatDXFRW));
    REQUIRE(liveCount(shown) == 5);
    REQUIRE(liveCount(reference) == 5);
    CHECK(order(shown) == order(reference));
}

TEST_CASE("A paper-space POLYLINE's VERTEX and SEQEND records are of the paper space",
          "[dxf][paper-space]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_seqend");
    const auto source = directory.file("layouts_r2000.dxf");
    writeText(source, layoutsDxf());
    RS_Graphic graphic;
    REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));
    RS_EntityContainer* active = graphic.paperSpaces().container(LC_PaperSpaceStore::activeName());
    auto* polyline = new RS_Polyline(active, RS_PolylineData(RS_Vector(false), RS_Vector(false), false));
    polyline->addVertex(RS_Vector(0., 0.));
    polyline->addVertex(RS_Vector(10., 0.));
    polyline->addVertex(RS_Vector(10., 10.));
    active->addEntity(polyline);

    const auto saved = directory.file("saved_r12.dxf");
    RS_FilterDXFRW writer;
    REQUIRE(writer.fileExport(graphic, lc::test::toQString(saved), RS2::FormatDXFRW12));
    // In ENTITIES, each VERTEX and SEQEND has the space of the POLYLINE before it.
    bool inEntities = false;
    bool ownerPaper = false;
    int vertices = 0;
    int sequenceEnds = 0;
    for (const DxfRecord& record : lc::test::dxfRecords(saved)) {
        const std::string& type = record.type();
        if (type == "SECTION") {
            const auto names = record.own(2);
            inEntities = !names.empty() && names.front() == "ENTITIES";
            continue;
        }
        if (!inEntities)
            continue;
        const auto space = record.own(67);
        const bool paper = !space.empty() && space.front() == "1";
        if (type == "POLYLINE") {
            ownerPaper = paper;
        } else if (type == "VERTEX" || type == "SEQEND") {
            CAPTURE(type);
            CHECK(paper == ownerPaper);
            if (ownerPaper)
                ++(type == "VERTEX" ? vertices : sequenceEnds);
        }
    }
    CHECK(vertices == 3);
    CHECK(sequenceEnds == 1);
}

namespace {
// The LAYOUT records of a written DXF, by name, with the handle of each and
// of the block record it names (its last 330), and the BLOCK_RECORDs by
// handle, with their name and the LAYOUT they point back at (340).
struct WrittenLayouts {
    struct Layout {
        std::string handle;
        std::string blockRecord;
    };
    struct BlockRecord {
        std::string name;
        std::string layout;
    };
    std::map<std::string, Layout> layouts;
    std::map<std::string, BlockRecord> blockRecords;
};

WrittenLayouts writtenLayouts(const std::filesystem::path& file) {
    WrittenLayouts written;
    for (const DxfRecord& record : lc::test::dxfRecords(file)) {
        const auto handles = record.own(5);
        if (handles.empty())
            continue;
        if (record.type() == "LAYOUT") {
            const auto names = record.own(1);
            const auto pointers = record.own(330);
            written.layouts[names.empty() ? std::string() : names.back()] = {
                handles.front(), pointers.empty() ? std::string() : pointers.back()};
        } else if (record.type() == "BLOCK_RECORD") {
            const auto names = record.own(2);
            const auto layouts = record.own(340);
            written.blockRecords[handles.front()] = {
                names.empty() ? std::string() : names.front(),
                layouts.empty() ? std::string() : layouts.front()};
        }
    }
    return written;
}
} // namespace

TEST_CASE("An R12 or R14 DXF holds no LAYOUT, and a LAYOUT names a block record written",
          "[dxf][paper-space]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_r14_layouts");
    const auto source = directory.file("layouts_r2000.dxf");
    writeText(source, layoutsDxf());
    RS_Graphic graphic;
    REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));

    // LAYOUT is an R2000 object: an R14 file that held one would name the
    // *Paper_Space0 block record it does not have, and a reader refuses it.
    const auto r14 = directory.file("saved_r14.dxf");
    RS_FilterDXFRW writer;
    REQUIRE(writer.fileExport(graphic, lc::test::toQString(r14), RS2::FormatDXFRW14));
    CHECK(writer.exportReport().contains(QStringLiteral("2 object(s) on paper-space layouts")));
    const WrittenLayouts inR14 = writtenLayouts(r14);
    CHECK(inR14.layouts.empty());
    for (const auto& [handle, record] : inR14.blockRecords)
        CHECK(record.layout.empty());
    CHECK(lc::test::fileBytes(r14).find("ACAD_LAYOUT") == std::string::npos);

    // In R2000 and later, each LAYOUT names a block record written, which
    // points back at it.
    for (const RS2::FormatType format : {RS2::FormatDXFRW2000, RS2::FormatDXFRW2018}) {
        CAPTURE(static_cast<int>(format));
        const auto saved = directory.file("saved_" + std::to_string(static_cast<int>(format)) + ".dxf");
        RS_FilterDXFRW modern;
        REQUIRE(modern.fileExport(graphic, lc::test::toQString(saved), format));
        const WrittenLayouts written = writtenLayouts(saved);
        REQUIRE(written.layouts.size() == 3);
        for (const auto& [name, layout] : written.layouts) {
            CAPTURE(name);
            const auto record = written.blockRecords.find(layout.blockRecord);
            REQUIRE(record != written.blockRecords.end());
            CHECK(record->second.layout == layout.handle);
        }
        CHECK(written.blockRecords.at(written.layouts.at("Model").blockRecord).name == "*Model_Space");
        CHECK(written.blockRecords.at(written.layouts.at("Layout1").blockRecord).name == "*Paper_Space");
        CHECK(written.blockRecords.at(written.layouts.at("Layout2").blockRecord).name == "*Paper_Space0");
    }
}

TEST_CASE("A LAYOUT whose paper space is not written is not written either",
          "[dxf][paper-space]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_layout_without_space");
    // The LAYOUTs are owned by a dictionary the file does not have, so no
    // dictionary written lists them: Layout2's *Paper_Space0 is not written,
    // and its LAYOUT, which would name it, is not either.
    std::string text = layoutsDxf();
    const std::string owned = "330\n1A\n100\nAcDbPlotSettings";
    for (std::size_t at = text.find(owned); at != std::string::npos; at = text.find(owned, at))
        text.replace(at, 7, "330\n99\n");
    const auto source = directory.file("orphan_layouts.dxf");
    writeText(source, text);
    RS_Graphic graphic;
    REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));
    REQUIRE(paperSpaceCount(graphic, QStringLiteral("*Paper_Space0")) == 2);

    const auto saved = directory.file("saved_r2018.dxf");
    RS_FilterDXFRW writer;
    REQUIRE(writer.fileExport(graphic, lc::test::toQString(saved), RS2::FormatDXFRW2018));
    CHECK(writer.exportReport().contains(QStringLiteral("2 object(s) on paper-space layouts")));
    const WrittenLayouts written = writtenLayouts(saved);
    CHECK(written.layouts.count("Layout2") == 0);
    CHECK(written.layouts.count("Layout1") == 1);
    for (const auto& [name, layout] : written.layouts) {
        CAPTURE(name);
        CHECK(written.blockRecords.count(layout.blockRecord) == 1);
    }
}

TEST_CASE("A DWG lists the dictionary of its layouts in its root",
          "[dxf][dwg][paper-space]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_dwg_root");
    const auto source = directory.file("layouts_r2000.dxf");
    writeText(source, layoutsDxf());
    RS_Graphic graphic;
    REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));

    const auto [format, name] = GENERATE(table<RS2::FormatType, const char*>(
        {{RS2::FormatDWG, "r2000"}, {RS2::FormatDWG2018, "r2018"}}));
    CAPTURE(name);
    const auto saved = directory.file(std::string("saved_") + name + ".dwg");
    RS_FilterDXFRW writer;
    REQUIRE(writer.fileExport(graphic, lc::test::toQString(saved), format));

    RS_Graphic reopened;
    REQUIRE(lc::test::importDrawing(reopened, saved, RS2::FormatDWG));
    const auto& metadata = reopened.dwgAdvancedMetadata();
    REQUIRE(metadata.layouts().size() == 3);
    const std::uint32_t layoutDictionary = metadata.layouts().front().parentHandle;
    for (const auto& layout : metadata.layouts())
        CHECK(layout.parentHandle == layoutDictionary);
    // A reader finds the layouts under ACAD_LAYOUT in the root, and nowhere
    // else.
    int roots = 0;
    bool listed = false;
    for (const auto& dictionary : metadata.dictionaries()) {
        if (dictionary.parentHandle != 0)
            continue;
        ++roots;
        for (const auto& entry : dictionary.entries) {
            if (entry.name == "ACAD_LAYOUT")
                listed = entry.handle == layoutDictionary;
        }
    }
    CHECK(roots == 1);
    CHECK(listed);
}

TEST_CASE("A layout a DWG cannot write is left out of a DWG save, and known to be before it",
          "[dxf][dwg][paper-space]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_dwg_rejected_layout");
    // Layout2's LAYOUT carries application data of its own, which a DWG
    // LAYOUT cannot hold: a DWG save writes neither it nor its paper space.
    std::string text = layoutsDxf();
    const std::string layout2 = "0\nLAYOUT\n5\n41\n";
    const auto at = text.find(layout2);
    REQUIRE(at != std::string::npos);
    text.insert(at + layout2.size(), "102\n{MY_APPLICATION\n1000\nkept\n102\n}\n");
    const auto source = directory.file("app_data.dxf");
    writeText(source, text);
    RS_Graphic graphic;
    REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));
    REQUIRE(paperSpaceCount(graphic, QStringLiteral("*Paper_Space0")) == 2);

    RS_FilterDXFRW filter;
    CHECK(filter.exportLeavesOut(graphic, RS2::FormatDXFRW2018).isEmpty());
    CHECK(filter.exportLeavesOut(graphic, RS2::FormatDWG2018) ==
          QStringLiteral("2 object(s) on paper-space layouts (Layout2)"));

    const auto saved = directory.file("saved_r2018.dwg");
    RS_FilterDXFRW writer;
    REQUIRE(writer.fileExport(graphic, lc::test::toQString(saved), RS2::FormatDWG2018));
    CHECK(writer.exportReport().contains(QStringLiteral("2 object(s) on paper-space layouts")));
    RS_Graphic reopened;
    REQUIRE(lc::test::importDrawing(reopened, saved, RS2::FormatDWG));
    CHECK(paperSpaceCount(reopened, QStringLiteral("*Paper_Space")) == 3);
    CHECK(paperSpaceCount(reopened, QStringLiteral("*Paper_Space0")) == 0);
}

TEST_CASE("A layer a deleted layout entity is on stays, for an undo to restore it",
          "[dxf][paper-space]") {
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    lc::test::OutputDirectory directory("lc_dxf_paper_space_deleted_layer");
    std::string text = layoutsDxf();
    const std::string modelInsert = "0\nINSERT\n5\n54\n330\n1F\n100\nAcDbEntity\n8\n0\n";
    const auto at = text.find(modelInsert);
    REQUIRE(at != std::string::npos);
    text.replace(at, modelInsert.size(),
                 "0\nINSERT\n5\n54\n330\n1E\n100\nAcDbEntity\n67\n1\n8\nFRAME\n");
    const auto source = directory.file("layouts_r2000.dxf");
    writeText(source, text);
    RS_Graphic graphic;
    REQUIRE(lc::test::importDrawing(graphic, source, RS2::FormatDXFRW));
    RS_Layer* layer = graphic.findLayer(QStringLiteral("FRAME"));
    REQUIRE(layer != nullptr);

    // As removing its block does: the INSERT is deleted, and an undo
    // restores it, on its layer.
    RS_EntityContainer* active = graphic.paperSpaces().container(LC_PaperSpaceStore::activeName());
    RS_Entity* insert = nullptr;
    for (RS_Entity* entity : *active) {
        if (entity->rtti() == RS2::EntityInsert)
            insert = entity;
    }
    REQUIRE(insert != nullptr);
    insert->setFlag(RS2::FlagDeleted);
    CHECK(graphic.paperSpaces().countOnLayer(QStringLiteral("FRAME")) == 1);
    CHECK_FALSE(graphic.removeLayer(layer));
    CHECK(graphic.findLayer(QStringLiteral("FRAME")) == layer);
    // The search for empty layers sees it too.
    bool seen = false;
    graphic.visitDrawingEntities([&seen, layer](RS_Entity* entity) {
        seen = seen || entity->getLayer() == layer;
    }, true);
    CHECK(seen);
}

// Converts one drawing for a look with another program:
//   LC_PAPER_SPACE_IN=<.dxf|.dwg> LC_PAPER_SPACE_OUT=<.dxf|.dwg> \
//   [LC_PAPER_SPACE_VERSION=r12|r14|r2000|r2007|r2018|dwg2000|...|dwg2018] \
//   <tests> "[.paper_space_convert]" -s
TEST_CASE("Paper space: convert a drawing to DXF or DWG", "[.paper_space_convert]") {
    const char* in = std::getenv("LC_PAPER_SPACE_IN");
    const char* out = std::getenv("LC_PAPER_SPACE_OUT");
    if (in == nullptr || out == nullptr)
        SKIP("LC_PAPER_SPACE_IN / LC_PAPER_SPACE_OUT not set");
    REQUIRE(lc::test::ensureSettings());
    lc::test::QuietLog quiet;
    const std::filesystem::path input(in);
    const bool dwg = input.extension() == ".dwg" || input.extension() == ".DWG";
    RS_Graphic graphic;
    REQUIRE(lc::test::importDrawing(graphic, input, dwg ? RS2::FormatDWG : RS2::FormatDXFRW));
    const std::string version =
        std::getenv("LC_PAPER_SPACE_VERSION") != nullptr ? std::getenv("LC_PAPER_SPACE_VERSION") : "";
    const std::map<std::string, RS2::FormatType> formats{
        {"r12", RS2::FormatDXFRW12},     {"r14", RS2::FormatDXFRW14},
        {"r2000", RS2::FormatDXFRW2000}, {"r2007", RS2::FormatDXFRW},
        {"r2018", RS2::FormatDXFRW2018}, {"dwg2000", RS2::FormatDWG},
        {"dwg2004", RS2::FormatDWG2004}, {"dwg2007", RS2::FormatDWG2007},
        {"dwg2010", RS2::FormatDWG2010}, {"dwg2013", RS2::FormatDWG2013},
        {"dwg2018", RS2::FormatDWG2018}};
    const auto chosen = formats.find(version);
    const RS2::FormatType format = chosen != formats.end() ? chosen->second : RS2::FormatDXFRW;
    RS_FilterDXFRW writer;
    REQUIRE(writer.fileExport(graphic, QString::fromLocal8Bit(out), format));
    std::cout << "model=" << liveCount(graphic) << " paper=" << graphic.paperSpaces().count();
    for (const auto& space : graphic.paperSpaces().spaces())
        std::cout << " " << space.name.toStdString() << "=" << liveCount(*space.entities);
    std::cout << "\nleft out: " << writer.exportReport().toStdString() << "\n";
}
