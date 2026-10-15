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

#include "lc_filtertestsupport.h"
#include "lc_paperspacestore.h"
#include "rs_block.h"
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
// block, whatever its name looks like.
std::string layoutsDxf(const bool withModelSpace = true) {
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
        if (name == "*Paper_Space0") {
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
    add({{0, "ENDSEC"}, {0, "SECTION"}, {2, "OBJECTS"}, {0, "DICTIONARY"}, {5, "C"},
         {330, "0"}, {100, "AcDbDictionary"}, {281, "1"}, {3, "ACAD_LAYOUT"}, {350, "1A"},
         {0, "DICTIONARY"}, {5, "1A"}, {330, "C"}, {100, "AcDbDictionary"}, {281, "1"},
         {3, "Layout1"}, {350, "26"}, {3, "Layout2"}, {350, "41"}, {3, "Model"}, {350, "22"}});
    for (const auto& [handle, name, tab, record] :
         {std::tuple<std::string, std::string, std::string, std::string>{"22", "Model", "0", "1F"},
          {"26", "Layout1", "1", "1E"},
          {"41", "Layout2", "2", "40"}}) {
        add({{0, "LAYOUT"}, {5, handle}, {330, "1A"}, {100, "AcDbPlotSettings"}, {1, ""},
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
        QStringLiteral("2 entity(ies) on paper-space layouts")));
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

TEST_CASE("Layout entities follow a renamed block and a removed layer",
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
    RS_Layer* layer = graphic.findLayer(QStringLiteral("FRAME"));
    REQUIRE(layer != nullptr);
    graphic.removeLayer(layer);

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
    CHECK(insert->getLayer()->getName() == QStringLiteral("0"));
}

// Converts one drawing for a look with another program:
//   LC_PAPER_SPACE_IN=<.dxf|.dwg> LC_PAPER_SPACE_OUT=<.dxf> \
//   [LC_PAPER_SPACE_VERSION=r12|r2000|r2007|r2018] <tests> "[.paper_space_convert]" -s
TEST_CASE("Paper space: convert a drawing to DXF", "[.paper_space_convert]") {
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
    const RS2::FormatType format = version == "r12"     ? RS2::FormatDXFRW12
                                   : version == "r2000" ? RS2::FormatDXFRW2000
                                   : version == "r2018" ? RS2::FormatDXFRW2018
                                                        : RS2::FormatDXFRW;
    RS_FilterDXFRW writer;
    REQUIRE(writer.fileExport(graphic, QString::fromLocal8Bit(out), format));
    std::cout << "model=" << liveCount(graphic) << " paper=" << graphic.paperSpaces().count();
    for (const auto& space : graphic.paperSpaces().spaces())
        std::cout << " " << space.name.toStdString() << "=" << liveCount(*space.entities);
    std::cout << "\nleft out: " << writer.exportReport().toStdString() << "\n";
}
