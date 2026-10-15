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

#include "lc_paperspacestore.h"

#include <QRegularExpression>

#include "rs_entity.h"
#include "rs_entitycontainer.h"
#include "rs_layer.h"

namespace {
// Exact block record names, as AutoCAD writes them (any letter case). R12
// names its spaces "$MODEL_SPACE" and "$PAPER_SPACE".
const QRegularExpression& modelSpaceName() {
    static const QRegularExpression expression(
        QStringLiteral("^[*$]model_space$"),
        QRegularExpression::CaseInsensitiveOption);
    return expression;
}

const QRegularExpression& paperSpaceName() {
    static const QRegularExpression expression(
        QStringLiteral("^[*$]paper_space\\d*$"),
        QRegularExpression::CaseInsensitiveOption);
    return expression;
}

const QRegularExpression& activePaperSpaceName() {
    static const QRegularExpression expression(
        QStringLiteral("^[*$]paper_space$"),
        QRegularExpression::CaseInsensitiveOption);
    return expression;
}
} // namespace

LC_PaperSpaceStore::LC_PaperSpaceStore(RS_EntityContainer* document)
    : m_document(document) {
}

LC_PaperSpaceStore::~LC_PaperSpaceStore() = default;

QString LC_PaperSpaceStore::activeName() {
    return QStringLiteral("*Paper_Space");
}

bool LC_PaperSpaceStore::isModelSpaceName(const QString& name) {
    return modelSpaceName().match(name).hasMatch();
}

bool LC_PaperSpaceStore::isPaperSpaceName(const QString& name) {
    return paperSpaceName().match(name).hasMatch();
}

bool LC_PaperSpaceStore::isActivePaperSpaceName(const QString& name) {
    return activePaperSpaceName().match(name).hasMatch();
}

LC_PaperSpaceStore::Space* LC_PaperSpaceStore::findSpace(const QString& name) {
    const QString key = isActivePaperSpaceName(name) ? activeName() : name;
    for (Space& space : m_spaces) {
        if (space.name.compare(key, Qt::CaseInsensitive) == 0)
            return &space;
    }
    return nullptr;
}

const LC_PaperSpaceStore::Space* LC_PaperSpaceStore::find(const QString& name) const {
    return const_cast<LC_PaperSpaceStore*>(this)->findSpace(name);
}

RS_EntityContainer* LC_PaperSpaceStore::container(const QString& name,
                                                  const std::uint32_t sourceHandle) {
    if (!isPaperSpaceName(name))
        return nullptr;
    Space* space = findSpace(name);
    if (space == nullptr) {
        Space added;
        added.name = isActivePaperSpaceName(name) ? activeName() : name;
        added.entities = std::make_unique<RS_EntityContainer>(m_document, true);
        // The active layout's paper space goes first: a save writes it first.
        const auto at = isActivePaperSpaceName(name) ? m_spaces.begin() : m_spaces.end();
        space = &*m_spaces.insert(at, std::move(added));
    }
    if (space->sourceHandle == 0)
        space->sourceHandle = sourceHandle;
    return space->entities.get();
}

unsigned LC_PaperSpaceStore::count() const {
    unsigned entities = 0;
    for (const Space& space : m_spaces) {
        for (const RS_Entity* entity : *space.entities) {
            if (entity != nullptr && !entity->getFlag(RS2::FlagDeleted))
                ++entities;
        }
    }
    return entities;
}

void LC_PaperSpaceStore::clear() {
    m_spaces.clear();
}

std::vector<std::unique_ptr<RS_Entity>> LC_PaperSpaceStore::takeEntities(const QString& name) {
    Space* space = findSpace(name);
    if (space == nullptr)
        return {};
    return space->entities->takeEntities();
}

void LC_PaperSpaceStore::renameInserts(const QString& oldName, const QString& newName) {
    for (Space& space : m_spaces)
        space.entities->renameInserts(oldName, newName);
}

void LC_PaperSpaceStore::moveEntitiesToLayerZero(const QString& layerName) {
    for (Space& space : m_spaces) {
        for (RS_Entity* entity : *space.entities) {
            if (entity == nullptr)
                continue;
            const RS_Layer* layer = entity->getLayer();
            if (layer != nullptr && layer->getName() == layerName)
                entity->setLayer(QStringLiteral("0"));
        }
    }
}
