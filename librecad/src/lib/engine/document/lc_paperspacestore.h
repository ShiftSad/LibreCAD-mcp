/****************************************************************************
**
** This file is part of the LibreCAD project, a 2D CAD program
**
** Copyright (C) 2026 LibreCAD www.librecad.org
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
****************************************************************************/

#ifndef LC_PAPERSPACESTORE_H
#define LC_PAPERSPACESTORE_H

#include <cstdint>
#include <memory>
#include <vector>

#include <QString>

class RS_Entity;
class RS_EntityContainer;

/**
 * What the paper spaces of a drawing's layouts hold.
 *
 * LibreCAD shows and edits model space only. The entities a DWG or DXF keeps
 * on its layouts are held here, one container per paper-space block record,
 * out of model space and out of the block list: nothing draws, selects or
 * edits them, and a save writes them back to their layouts
 * (RS_FilterDXFRW).
 *
 * "*Paper_Space" is the paper space of the active layout, and the only one an
 * R12 or R14 drawing has; "*Paper_Space0", "*Paper_Space1", ... are those of
 * the other layouts. The containers are parented to the document, so their
 * entities find its layers and blocks.
 */
class LC_PaperSpaceStore {
public:
    struct Space {
        /** Name of the paper-space block record ("*Paper_Space", ...). */
        QString name;
        /** Handle of that block record in the file read, 0 if unknown. */
        std::uint32_t sourceHandle = 0;
        std::unique_ptr<RS_EntityContainer> entities;
    };

    explicit LC_PaperSpaceStore(RS_EntityContainer* document);
    ~LC_PaperSpaceStore();
    LC_PaperSpaceStore(const LC_PaperSpaceStore&) = delete;
    LC_PaperSpaceStore& operator=(const LC_PaperSpaceStore&) = delete;

    /** The name of the active layout's paper space, "*Paper_Space". */
    static QString activeName();
    /** @return true for a model-space block record name: "*Model_Space", or
     *  the "$MODEL_SPACE" of an R12 file, in any letter case. */
    static bool isModelSpaceName(const QString& name);
    /** @return true for a paper-space block record name: "*Paper_Space",
     *  "*Paper_Space0", "*Paper_Space1", ..., or the "$PAPER_SPACE" of an R12
     *  file, in any letter case. A block named "*Paper_Space_Frame" is not. */
    static bool isPaperSpaceName(const QString& name);
    /** @return true for the active layout's paper space ("*Paper_Space",
     *  "$PAPER_SPACE"). */
    static bool isActivePaperSpaceName(const QString& name);

    /**
     * @return the container of the paper space named @p name, made on first
     * use. @p name must satisfy isPaperSpaceName(); the R12 "$PAPER_SPACE" is
     * the active one. A known @p sourceHandle is kept with it.
     */
    RS_EntityContainer* container(const QString& name, std::uint32_t sourceHandle = 0);
    /** @return the paper space named @p name, or nullptr. */
    const Space* find(const QString& name) const;
    /** The paper spaces, the active one first, then in the order read. */
    const std::vector<Space>& spaces() const { return m_spaces; }

    /** Number of entities over all paper spaces, deleted ones left out. */
    unsigned count() const;
    bool isEmpty() const { return count() == 0; }
    /** Deletes every paper space and its entities. */
    void clear();
    /**
     * Takes the entities of the paper space named @p name out of it, in their
     * order and without a parent, and hands them to the caller. The paper
     * space stays, empty.
     */
    std::vector<std::unique_ptr<RS_Entity>> takeEntities(const QString& name);

    /** Renames the block a paper-space INSERT refers to. */
    void renameInserts(const QString& oldName, const QString& newName);
    /** Number of entities on the layer named @p layerName, deleted ones
     *  included: an undo may restore those, on that layer. */
    unsigned countOnLayer(const QString& layerName) const;

    /** Number of VIEWPORT entities the layouts of the file read had. LibreCAD
     *  reads none of them, and a save writes the layouts without them. */
    unsigned viewportsNotRead() const { return m_viewportsNotRead; }
    void noteViewportNotRead() { ++m_viewportsNotRead; }

private:
    Space* findSpace(const QString& name);

    RS_EntityContainer* m_document = nullptr;
    std::vector<Space> m_spaces;
    unsigned m_viewportsNotRead = 0;
};

#endif
