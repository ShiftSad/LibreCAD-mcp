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

// AutoCAD ends an R2007+ CLASSES section with the class string stream, its
// footer (the string size word and an end bit), padding to a byte boundary,
// the CRC and then the END sentinel. The reader used to look for the CRC
// where the string data ends, in front of that footer, and so reported a
// CLASSES CRC mismatch for every R2007+ drawing, including AutoCAD's own.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "intern/dwgbuffer.h"
#include "intern/dwgbufferw.h"
#include "intern/dwgreader21.h"
#include "intern/dwgreader24.h"
#include "intern/dwgreader27.h"
#include "intern/dwgreader32.h"
#include "intern/dwgutil.h"

namespace {

// Feeds the CLASSES parser one uncompressed AC18 data page.
template <typename Reader>
class ClassesCrcProbe final : public Reader {
public:
    explicit ClassesCrcProbe(std::vector<std::uint8_t>& page)
        : Reader(std::make_unique<dwgBuffer>(page.data(), page.size()),
                 nullptr) {}

    void setClassesPage(DRW::Version value, std::uint8_t maintenanceVersion,
                        std::uint64_t logicalSize) {
        this->version = value;
        this->appMaintenanceVersion = maintenanceVersion;
        auto& section = this->sections[secEnum::CLASSES];
        section.Id = 1;
        section.size = logicalSize;
        section.maxSize = this->fileBuf->size() - 32;
        section.compressed = 1;
        section.pageCount = 1;
        section.pages.emplace(1, dwgPageInfo(1, 0, this->fileBuf->size()));
    }
};

void writeLittleEndian32(std::uint8_t* bytes, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::vector<std::uint8_t> makeDataPage(const std::vector<std::uint8_t>& payload) {
    const auto size = static_cast<std::uint32_t>(payload.size());
    const std::uint32_t dataChecksum =
        dwgUtil::checksum18(0, payload.data(), payload.size());
    dwgBufferW header;
    header.putRawLong32(0x4163043B); // data page type
    header.putRawLong32(1);          // section number
    header.putRawLong32(size);       // compressed size
    header.putRawLong32(size);       // uncompressed size
    header.putRawLong32(0);          // start offset
    header.putRawLong32(0);
    header.putRawLong32(0);          // header checksum, patched below
    header.putRawLong32(dataChecksum);
    auto bytes = header.data();
    writeLittleEndian32(bytes.data() + 24,
                        dwgUtil::checksum18(dataChecksum, bytes.data(),
                                            bytes.size()));
    dwgCompressor::decrypt18Hdr(bytes.data(), bytes.size(), 0);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

struct ClassesSection {
    std::vector<std::uint8_t> bytes;
    std::size_t crcOffset = 0;
    std::size_t stringBits = 0;
};

// Lays out a CLASSES section the way AutoCAD 2007+ saves it: RL size, the
// optional RL high size word, RL bit size, class data, string stream, string
// stream footer and end bit, padding, CRC, END sentinel and eight unknown
// bytes. A string stream of more than 0x7FFF bits stores its size in two
// words, the high word in front of the flagged low word.
ClassesSection makeAutoCadClassesSection(DRW::Version version, bool hasHSize,
                                         int classCount = 1) {
    dwgBufferW section;
    section.putBytes(dwgSentinels::CLASSES_BEGIN, 16);
    const std::size_t sizeOffset = section.size();
    section.putRawLong32(0);
    if (hasHSize)
        section.putRawLong32(0);
    const std::size_t bitSizeOffset = section.size();
    section.putRawLong32(0);

    dwgBufferW strings;
    // maximum class number
    section.putBitShort(static_cast<std::uint16_t>(499 + classCount));
    section.putRawChar8(0);
    section.putRawChar8(0);
    section.putBit(1);
    for (int classIndex = 0; classIndex < classCount; ++classIndex) {
        const std::string suffix =
            classIndex == 0 ? std::string() : std::to_string(classIndex);
        section.putBitShort(static_cast<std::uint16_t>(500 + classIndex));
        section.putBitShort(0x401);
        strings.putVariableText(version, "TEST_APP");
        strings.putVariableText(version, "AcDbTestEntity" + suffix);
        strings.putVariableText(version, "TEST_ENTITY" + suffix);
        section.putBit(0);
        section.putBitShort(0x1F2);
        for (int i = 0; i < 5; ++i)
            section.putBitLong(0);
    }

    section.alignToByte();
    strings.alignToByte();
    section.putBytes(strings.data().data(), strings.size());
    const std::size_t stringBits = strings.size() * 8;
    if (stringBits > 0x7FFF) {
        section.putRawShort16(static_cast<std::uint16_t>(stringBits >> 15));
        section.putRawShort16(
            static_cast<std::uint16_t>((stringBits & 0x7FFF) | 0x8000));
    } else {
        section.putRawShort16(static_cast<std::uint16_t>(stringBits));
    }
    section.putBit(1);
    // The bit size counts from its own field through the end bit.
    section.patchRawLong32(
        bitSizeOffset,
        section.bitCount() - static_cast<std::uint32_t>(bitSizeOffset * 8));
    section.alignToByte();
    // The byte size counts from the bit size field up to the CRC.
    section.patchRawLong32(
        sizeOffset, static_cast<std::uint32_t>(section.size() - bitSizeOffset));

    ClassesSection result;
    result.crcOffset = section.size();
    result.stringBits = stringBits;
    section.putRawShort16(section.crc16(0xC0C1, 16, section.size()));
    section.putBytes(dwgSentinels::CLASSES_END, 16);
    const std::vector<std::uint8_t> unknown(8, 0);
    section.putBytes(unknown.data(), unknown.size());
    result.bytes = section.data();
    return result;
}

template <typename Reader>
std::size_t classesCrcMismatches(const std::vector<std::uint8_t>& section,
                                 DRW::Version version,
                                 std::uint8_t maintenanceVersion) {
    auto page = makeDataPage(section);
    ClassesCrcProbe<Reader> reader(page);
    reader.setClassesPage(version, maintenanceVersion, section.size());
    REQUIRE(reader.readDwgClasses());
    return reader.m_classesCrcMismatch;
}

template <typename Reader>
void checkClassesCrc(DRW::Version version, std::uint8_t maintenanceVersion,
                     bool hasHSize, int classCount = 1) {
    const ClassesSection section =
        makeAutoCadClassesSection(version, hasHSize, classCount);
    // Only a stream of more than 4 KiB of strings uses the two-word size.
    REQUIRE((section.stringBits > 0x7FFF) == (classCount > 1));
    const auto storedCrc = [&section](const std::vector<std::uint8_t>& bytes) {
        return bytes[section.crcOffset]
            | (bytes[section.crcOffset + 1] << 8);
    };
    // A zero CRC is treated as absent, which would hide a misplaced read.
    REQUIRE(storedCrc(section.bytes) != 0);
    CHECK(classesCrcMismatches<Reader>(section.bytes, version,
                                       maintenanceVersion) == 0);

    // A damaged CRC at that position is still reported, as a warning only.
    auto damaged = section.bytes;
    damaged[section.crcOffset] ^= 0xFF;
    REQUIRE(storedCrc(damaged) != 0);
    CHECK(classesCrcMismatches<Reader>(damaged, version,
                                       maintenanceVersion) == 1);
}

} // namespace

TEST_CASE("R2007 CLASSES CRC is read after the string stream footer",
          "[dwg][classes]") {
    checkClassesCrc<dwgReader21>(DRW::AC1021, 0, false);
}

TEST_CASE("R2010+ CLASSES CRC is read after the string stream footer",
          "[dwg][classes]") {
    SECTION("AC1024 without the high size word") {
        // Maintenance versions up to 3 omit the RL high size word.
        checkClassesCrc<dwgReader24>(DRW::AC1024, 2, false);
    }
    SECTION("AC1024 with the high size word") {
        checkClassesCrc<dwgReader24>(DRW::AC1024, 8, true);
    }
    SECTION("AC1027") {
        checkClassesCrc<dwgReader27>(DRW::AC1027, 8, true);
    }
    SECTION("AC1032") {
        // AutoCAD 2018 through 2026 always write the high size word.
        checkClassesCrc<dwgReader32>(DRW::AC1032, 0, true);
    }
}

TEST_CASE("CLASSES CRC is found after a two-word string stream size",
          "[dwg][classes]") {
    // More than 4 KiB of class strings needs a second string size word in
    // the footer, in front of the flagged low word and the end bit.
    constexpr int classCount = 64;
    SECTION("AC1021") {
        checkClassesCrc<dwgReader21>(DRW::AC1021, 0, false, classCount);
    }
    SECTION("AC1024") {
        checkClassesCrc<dwgReader24>(DRW::AC1024, 8, true, classCount);
    }
    SECTION("AC1032") {
        checkClassesCrc<dwgReader32>(DRW::AC1032, 0, true, classCount);
    }
}
