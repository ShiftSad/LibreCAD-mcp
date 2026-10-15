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

#include "lc_remoteapientityops.h"

#include <algorithm>
#include <cmath>

#include <QMap>
#include <QSet>

#include "lc_containertraverser.h"
#include "lc_graphicviewport.h"
#include "lc_remoteapientityfactory.h"
#include "lc_remoteapientityindex.h"
#include "lc_remoteapientityjson.h"
#include "lc_remoteapijson.h"
#include "lc_remoteapitypes.h"
#include "lc_splinepoints.h"
#include "rs_arc.h"
#include "rs_circle.h"
#include "rs_document.h"
#include "rs_ellipse.h"
#include "rs_entity.h"
#include "rs_graphic.h"
#include "rs_hatch.h"
#include "rs_information.h"
#include "rs_insert.h"
#include "rs_layer.h"
#include "rs_line.h"
#include "rs_math.h"
#include "rs_mtext.h"
#include "rs_polyline.h"
#include "rs_selection.h"
#include "rs_spline.h"
#include "rs_text.h"

namespace {

namespace json = LC_RemoteApiEntityJson;

QJsonArray idsOf(const QList<RS_Entity*>& entities) {
    QJsonArray ids;
    for (const RS_Entity* e : entities) {
        ids.append(json::idValue(e));
    }
    return ids;
}

QJsonArray idsArray(const QList<unsigned long long>& ids) {
    QJsonArray out;
    for (const unsigned long long id : ids) {
        out.append(static_cast<qint64>(id));
    }
    return out;
}

QJsonArray entitiesJson(const QList<RS_Entity*>& entities, const json::Detail detail) {
    QJsonArray out;
    for (RS_Entity* e : entities) {
        out.append(json::entityToJson(e, detail));
    }
    return out;
}

RS_Pen byLayerPen() {
    return RS_Pen(RS_Color(RS2::FlagByLayer), RS2::WidthByLayer, RS2::LineByLayer);
}

/** 409 entity_locked with the ids of the entities on locked layers, if any. */
void refuseLocked(const QList<RS_Entity*>& entities) {
    QJsonArray locked;
    QStringList layers;
    for (const RS_Entity* e : entities) {
        if (e->isLocked()) {
            locked.append(json::idValue(e));
            const RS_Layer* layer = e->getLayer(true);
            if (layer != nullptr && !layers.contains(layer->getName())) {
                layers << layer->getName();
            }
        }
    }
    if (!locked.isEmpty()) {
        throw LC_ApiError(409, QStringLiteral("entity_locked"),
                          QStringLiteral("%1 of the entities are on a locked layer (%2); nothing was changed")
                              .arg(locked.size())
                              .arg(layers.join(QStringLiteral(", "))),
                          {{QStringLiteral("ids"), locked},
                           {QStringLiteral("hint"), QStringLiteral("unlock the layer first (update_layer locked=false)")}});
    }
}

void refuseLockedLayer(const RS_Layer* layer) {
    if (layer != nullptr && layer->isLocked()) {
        throw LC_ApiError(409, QStringLiteral("layer_locked"), QStringLiteral("layer '%1' is locked").arg(layer->getName()),
                          {{QStringLiteral("layer"), layer->getName()},
                           {QStringLiteral("hint"), QStringLiteral("unlock the layer first (update_layer locked=false)")}});
    }
}

/** Errors of one spec that a create request reports together as 422 invalid_entity. */
bool isSpecError(const LC_ApiError& e) {
    return e.status == 400 || e.token == QLatin1String("invalid_entity");
}

void recordSpecError(const LC_ApiError& e, const QString& specPath, QJsonObject& errors, QJsonObject& allowed) {
    const QJsonValue nested = e.extra.value(QStringLiteral("errors"));
    if (nested.isObject()) {
        const QJsonObject nestedErrors = nested.toObject();
        for (auto it = nestedErrors.constBegin(); it != nestedErrors.constEnd(); ++it) {
            errors.insert(it.key(), it.value());
        }
        return;
    }
    QString key = e.extra.value(QStringLiteral("param")).toString();
    if (key.isEmpty()) {
        key = e.extra.value(QStringLiteral("field")).toString();
    }
    if (key.isEmpty()) {
        key = specPath;
    }
    QString message = e.detail.isEmpty() ? e.token : e.detail;
    if (e.token == QLatin1String("unknown_field") && !message.contains(QLatin1String("unknown")) && !message.contains(QLatin1String("not a field"))) {
        message = QStringLiteral("unknown field: ") + message;
    }
    const QJsonValue allowedValues = e.extra.value(QStringLiteral("allowed"));
    if (allowedValues.isArray()) {
        allowed.insert(key, allowedValues);
    }
    errors.insert(key, message);
}

/** The atomic pieces of an entity (itself, or the leaves of a container). */
std::vector<RS_Entity*> atomicPieces(RS_Entity* e) {
    if (e->isContainer()) {
        return lc::LC_ContainerTraverser{*static_cast<RS_EntityContainer*>(e), RS2::ResolveAll}.entities();
    }
    return {e};
}

double piecesLength(const RS_EntityContainer* container) {
    double length = 0.0;
    for (const RS_Entity* piece : lc::LC_ContainerTraverser{*container, RS2::ResolveAll}.entities()) {
        const double l = piece->getLength();
        if (l > 0.0) {
            length += l;
        }
    }
    return length;
}

/** Area and perimeter of a closed entity; false for an open one. */
bool closedArea(RS_Entity* e, double& area, double& perimeter) {
    switch (e->rtti()) {
        case RS2::EntityCircle: {
            const double r = e->getRadius();
            area = M_PI * r * r;
            perimeter = 2.0 * M_PI * r;
            return true;
        }
        case RS2::EntityEllipse: {
            const auto* ellipse = static_cast<RS_Ellipse*>(e);
            if (!json::isFullEllipse(ellipse)) {
                return false;
            }
            // pi a b: a full ellipse stored with equal nonzero angles is an "arc" to LibreCAD
            area = M_PI * ellipse->getMajorRadius() * ellipse->getMinorRadius();
            perimeter = ellipse->getLength();
            return true;
        }
        case RS2::EntityPolyline: {
            const auto* polyline = static_cast<RS_Polyline*>(e);
            if (!polyline->isClosed()) {
                return false;
            }
            area = json::closedPolylineArea(polyline);
            perimeter = json::polylineLength(polyline);
            return true;
        }
        case RS2::EntitySpline: {
            const auto* spline = static_cast<RS_Spline*>(e);
            if (!spline->isClosed()) {
                return false;
            }
            area = std::abs(spline->areaLineIntegral());
            perimeter = piecesLength(spline);
            return true;
        }
        case RS2::EntitySplinePoints: {
            const auto* spline = static_cast<LC_SplinePoints*>(e);
            if (!spline->isClosed()) {
                return false;
            }
            area = std::abs(spline->areaLineIntegral());
            perimeter = spline->getLength();
            return true;
        }
        case RS2::EntityHatch: {
            const auto* hatch = static_cast<RS_Hatch*>(e);
            area = hatch->getTotalArea();
            perimeter = 0.0;
            // every loop, holes included: countLoops() counts the outer loops only
            for (int i = 0; i < hatch->countAllLoops(); ++i) {
                const RS_EntityContainer* loop = hatch->getBoundaryContainer(i);
                if (loop != nullptr) {
                    perimeter += piecesLength(loop);
                }
            }
            return true;
        }
        default:
            return false;
    }
}

/** Whether measure kind=length applies: curves only (text, hatches, inserts and points have no length). */
bool hasLength(const RS_Entity* e) {
    switch (e->rtti()) {
        case RS2::EntityLine:
        case RS2::EntityArc:
        case RS2::EntityCircle:
        case RS2::EntityEllipse:
        case RS2::EntityPolyline:
        case RS2::EntitySpline:
        case RS2::EntitySplinePoints:
        case RS2::EntityParabola:
        case RS2::EntityHyperbola:
            return true;
        default:
            return false;
    }
}

bool isCurved(const RS_Entity* e) {
    switch (e->rtti()) {
        case RS2::EntityArc:
        case RS2::EntityCircle:
        case RS2::EntityEllipse:
        case RS2::EntitySpline:
        case RS2::EntitySplinePoints:
        case RS2::EntityParabola:
        case RS2::EntityHyperbola:
            return true;
        default:
            return false;
    }
}

/**
 * Why e cannot follow a non-uniform scale, or empty when it can. LibreCAD
 * applies only the x factor to text heights and to the arcs of hatch loops,
 * and turns polyline arcs into elliptic segments no bulge can describe.
 */
QString nonUniformScaleRefusal(RS_Entity* e) {
    switch (e->rtti()) {
        case RS2::EntityText:
        case RS2::EntityMText:
            return QStringLiteral("text cannot be scaled by different x and y factors; scale it uniformly");
        case RS2::EntityPolyline:
            for (const RS_Entity* segment : *static_cast<RS_Polyline*>(e)) {
                if (segment != nullptr && isCurved(segment)) {
                    return QStringLiteral("a polyline with arc segments cannot be scaled by different x and y factors; "
                                          "explode it first, or scale it uniformly");
                }
            }
            return {};
        case RS2::EntityHatch: {
            const auto* hatch = static_cast<RS_Hatch*>(e);
            for (int i = 0; i < hatch->countAllLoops(); ++i) {
                const RS_EntityContainer* loop = hatch->getBoundaryContainer(i);
                if (loop == nullptr) {
                    continue;
                }
                for (const RS_Entity* piece : lc::LC_ContainerTraverser{*loop, RS2::ResolveAll}.entities()) {
                    if (piece != nullptr && isCurved(piece)) {
                        return QStringLiteral("a hatch with curved boundary edges cannot be scaled by different x and y "
                                              "factors; delete it and hatch the scaled boundary again");
                    }
                }
            }
            return {};
        }
        case RS2::EntityDimRadial:
        case RS2::EntityDimDiametric:
            return QStringLiteral("a radial or diametric dimension cannot be scaled by different x and y factors");
        default:
            return {};
    }
}

double angleBetween(const RS_Vector& d1, const RS_Vector& d2) {
    const double m = d1.magnitude() * d2.magnitude();
    const double c = std::clamp(RS_Vector::dotP(d1, d2) / m, -1.0, 1.0);
    return RS_Math::rad2deg(std::acos(c));
}

/** The transform of a POST /entities/transform body; apply() moves the n-th copy n steps. */
struct TransformSpec {
    QString op;
    RS_Vector offset;
    RS_Vector center;
    double angle = 0.0;     // radians
    RS_Vector base;
    RS_Vector factor{1.0, 1.0};
    RS_Vector axis1;
    RS_Vector axis2;

    bool isotropic() const {
        return std::abs(factor.x - factor.y) < RS_TOLERANCE;
    }

    void apply(RS_Entity* e, const int n) const {
        if (op == QLatin1String("move")) {
            e->move(offset * n);
        }
        else if (op == QLatin1String("rotate")) {
            e->rotate(center, angle * n);
        }
        else if (op == QLatin1String("scale")) {
            e->scale(base, RS_Vector(std::pow(factor.x, n), std::pow(factor.y, n)));
        }
        else {
            e->mirror(axis1, axis2);
        }
    }
};

/**
 * The source to clone for a transform of e: e itself, or for a non-isotropic
 * scale of a circle or arc the ellipse it becomes (RS_Modification::scale does
 * the same), with e's identity.
 */
RS_Entity* transformSource(RS_Entity* e, const TransformSpec& spec, std::unique_ptr<RS_Entity>& temporary) {
    if (spec.op != QLatin1String("scale") || spec.isotropic()) {
        return e;
    }
    if (e->rtti() == RS2::EntityCircle) {
        const auto* c = static_cast<RS_Circle*>(e);
        temporary = std::make_unique<RS_Ellipse>(nullptr, RS_EllipseData{c->getCenter(), {c->getRadius(), 0.}, 1., 0., 0., false});
    }
    else if (e->rtti() == RS2::EntityArc) {
        const auto* a = static_cast<RS_Arc*>(e);
        temporary = std::make_unique<RS_Ellipse>(
            nullptr, RS_EllipseData{a->getCenter(), {a->getRadius(), 0.}, 1.0, a->getAngle1(), a->getAngle2(), a->isReversed()});
    }
    else {
        return e;
    }
    LC_RemoteApiEntityFactory::transplantIdentity(e, temporary.get());
    return temporary.get();
}

/** Copies the API makes get a lineage of their own (design 2.6), except those LC_UndoSection renews. */
void renewCopyLineage(RS_Entity* copy) {
    if (copy->rtti() != RS2::EntityInsert && copy->ownerLineage() == 0) {
        copy->setLineage(RS_Entity::newLineage());
    }
}

QString countText(const int n, const char* one, const char* many) {
    return n == 1 ? QStringLiteral("1 %1").arg(QLatin1String(one)) : QStringLiteral("%1 %2").arg(n).arg(QLatin1String(many));
}

} // namespace

LC_RemoteApiEntityOps::LC_RemoteApiEntityOps(RS_Document* document, LC_GraphicViewport* viewport)
    : m_document(document)
    , m_graphic(document != nullptr ? document->getGraphic() : nullptr)
    , m_viewport(viewport) {
    if (m_document == nullptr || m_viewport == nullptr) {
        throw LC_ApiError(500, QStringLiteral("internal"), QStringLiteral("the document has no view"));
    }
}

RS_Layer* LC_RemoteApiEntityOps::targetLayer(const QString& name) const {
    if (!name.isEmpty()) {
        return LC_RemoteApiEntityIndex::requireLayer(m_graphic, name);
    }
    RS_Layer* active = m_graphic != nullptr ? m_graphic->getActiveLayer() : nullptr;
    if (active == nullptr) {
        active = LC_RemoteApiEntityIndex::requireLayer(m_graphic, QStringLiteral("0"));
    }
    return active;
}

QString LC_RemoteApiEntityOps::describeCreated(const QList<RS_Entity*>& entities) {
    QStringList layers;
    for (const RS_Entity* e : entities) {
        const RS_Layer* layer = e->getLayer(true);
        const QString name = layer != nullptr ? layer->getName() : QString();
        if (!layers.contains(name)) {
            layers << name;
        }
    }
    QString line = QStringLiteral("[MCP] created ") + countText(static_cast<int>(entities.size()), "entity", "entities");
    if (layers.size() == 1) {
        line += QStringLiteral(" on layer '%1'").arg(layers.constFirst());
    }
    else if (layers.size() > 1) {
        line += QStringLiteral(" on %1 layers").arg(layers.size());
    }
    return line;
}

QJsonObject LC_RemoteApiEntityOps::list(const QMultiHash<QString, QString>& query) const {
    LC_RemoteApiQuery::allowOnly(query, {QStringLiteral("type"), QStringLiteral("layer"), QStringLiteral("bbox"), QStringLiteral("bbox_mode"),
                                         QStringLiteral("selected"), QStringLiteral("include_hidden"), QStringLiteral("limit"),
                                         QStringLiteral("offset"), QStringLiteral("detail")});
    const LC_RemoteApiEntityFilter filter = LC_RemoteApiEntityFilter::fromQuery(query, m_graphic);
    const LC_RemoteApiPage page = LC_RemoteApiPage::fromQuery(query);
    json::Detail detail = json::Detail::Summary;
    if (const auto value = LC_RemoteApiQuery::single(query, QStringLiteral("detail"))) {
        if (*value == QLatin1String("full")) {
            detail = json::Detail::Full;
        }
        else if (*value != QLatin1String("summary")) {
            json::throwBadParam(QStringLiteral("detail"), QStringLiteral("'detail' must be summary or full"),
                                QJsonArray{QStringLiteral("summary"), QStringLiteral("full")});
        }
    }

    const LC_RemoteApiEntityIndex index(m_document);
    const QList<RS_Entity*> matched = index.query(filter);
    const int total = static_cast<int>(matched.size());
    QJsonArray entities;
    for (int i = page.offset; i < total && i < page.offset + page.limit; ++i) {
        entities.append(json::entityToJson(matched.at(i), detail));
    }
    const int next = page.offset + page.limit;
    return {{QStringLiteral("entities"), entities},
            {QStringLiteral("total"), total},
            {QStringLiteral("offset"), page.offset},
            {QStringLiteral("limit"), page.limit},
            {QStringLiteral("next_offset"), next < total ? QJsonValue(next) : QJsonValue(QJsonValue::Null)}};
}

QJsonObject LC_RemoteApiEntityOps::get(const unsigned long long id) const {
    const LC_RemoteApiEntityIndex index(m_document);
    return {{QStringLiteral("entity"), json::entityToJson(index.require(id), json::Detail::Full)}};
}

QList<RS_Entity*> LC_RemoteApiEntityOps::addEntities(std::vector<std::unique_ptr<RS_Entity>>& entities, const QList<RS_Layer*>& layers,
                                                     const QList<RS_Pen>& pens, const bool select, QJsonArray& warnings) const {
    QList<RS_Layer*> checked;
    for (RS_Layer* layer : layers) {
        if (checked.contains(layer)) {
            continue;
        }
        checked.append(layer);
        refuseLockedLayer(layer);
        if (layer != nullptr && layer->isFrozen()) {
            warnings.append(QStringLiteral("layer_frozen: layer '%1' is frozen, so the new entities on it are hidden").arg(layer->getName()));
        }
    }
    QList<RS_Entity*> added;
    m_document->undoableModify(
        m_viewport,
        [&](LC_DocumentModificationBatch& ctx) {
            ctx.dontSetActiveLayerAndPen();
            for (size_t i = 0; i < entities.size(); ++i) {
                RS_Entity* e = entities[i].release();
                e->setLayer(layers.at(static_cast<int>(i)));
                e->setPen(pens.at(static_cast<int>(i)));
                ctx += e;
                added.append(e);
            }
            return !added.isEmpty();
        },
        [select](LC_DocumentModificationBatch& ctx, RS_Document* doc) {
            if (select && ctx.success) {
                doc->select(ctx.entitiesToAdd);
            }
        });
    entities.clear();
    return added;
}

QJsonObject LC_RemoteApiEntityOps::create(const LC_JsonReader& body, QJsonArray& warnings, QString& audit) const {
    body.allowOnly({"entities", "layer", "select"});
    const QJsonObject raw = body.raw();
    const QJsonValue specsValue = raw.value(QStringLiteral("entities"));
    if (specsValue.isUndefined()) {
        json::throwMissingParam(QStringLiteral("entities"));
    }
    if (!specsValue.isArray()) {
        json::throwBadParam(QStringLiteral("entities"), QStringLiteral("'entities' must be an array of entity specs"));
    }
    const QJsonArray specs = specsValue.toArray();
    if (specs.isEmpty()) {
        json::throwBadParam(QStringLiteral("entities"), QStringLiteral("'entities' must hold at least one entity spec"));
    }
    if (specs.size() > MaxBatch) {
        throw LC_ApiError(422, QStringLiteral("too_many"), QStringLiteral("at most %1 entities per request").arg(MaxBatch),
                          {{QStringLiteral("limit"), MaxBatch}});
    }
    const QString bodyLayer = body.str("layer", QString());
    const bool select = body.flag("select", false);
    RS_Layer* defaultLayer = targetLayer(bodyLayer);

    const LC_RemoteApiEntityIndex index(m_document);
    const LC_RemoteApiEntityFactory factory(m_document, &index);
    std::vector<std::unique_ptr<RS_Entity>> built;
    QList<RS_Layer*> layers;
    QList<RS_Pen> pens;
    QJsonObject errors;
    QJsonObject allowed;
    QStringList notes;
    int invalidSpecs = 0;
    for (int i = 0; i < specs.size(); ++i) {
        const QString path = json::indexPath(QStringLiteral("entities"), i);
        if (!specs.at(i).isObject()) {
            errors.insert(path, QStringLiteral("must be an entity spec object"));
            ++invalidSpecs;
            continue;
        }
        const QJsonObject spec = specs.at(i).toObject();
        try {
            const json::Attributes attrs = json::Attributes::read(spec, path);
            RS_Layer* layer = attrs.layer.has_value() ? targetLayer(*attrs.layer) : defaultLayer;
            std::unique_ptr<RS_Entity> e = factory.create(spec, path, notes);
            built.push_back(std::move(e));
            layers.append(layer);
            pens.append(attrs.applyTo(byLayerPen()));
        }
        catch (const LC_ApiError& e) {
            if (!isSpecError(e)) {
                throw;
            }
            recordSpecError(e, path, errors, allowed);
            ++invalidSpecs;
        }
    }
    if (!errors.isEmpty()) {
        QJsonObject extra{{QStringLiteral("errors"), errors}};
        if (!allowed.isEmpty()) {
            extra.insert(QStringLiteral("allowed"), allowed);
        }
        throw LC_ApiError(422, QStringLiteral("invalid_entity"),
                          QStringLiteral("%1 of %2 entity specs are invalid; nothing was created").arg(invalidSpecs).arg(specs.size()), extra);
    }
    for (const QString& note : std::as_const(notes)) {
        warnings.append(note);
    }

    const QList<RS_Entity*> added = addEntities(built, layers, pens, select, warnings);
    audit = describeCreated(added);
    return {{QStringLiteral("created"), entitiesJson(added, json::Detail::Full)},
            {QStringLiteral("ids"), idsOf(added)},
            {QStringLiteral("undoable"), true}};
}

QJsonObject LC_RemoteApiEntityOps::update(const unsigned long long id, const LC_JsonReader& body, QJsonArray& warnings, QString& audit) const {
    const QJsonObject raw = body.raw();
    if (raw.isEmpty()) {
        json::throwBadParam(QStringLiteral("body"), QStringLiteral("give at least one field to change"));
    }
    const LC_RemoteApiEntityIndex index(m_document);
    RS_Entity* original = index.require(id);
    const json::Attributes attrs = json::Attributes::read(raw, QString());
    RS_Layer* newLayer = attrs.layer.has_value() ? targetLayer(*attrs.layer) : nullptr;

    const LC_RemoteApiEntityFactory factory(m_document, &index);
    QStringList notes;
    std::unique_ptr<RS_Entity> replacement = factory.patch(original, raw, notes);
    refuseLocked({original});
    refuseLockedLayer(newLayer);
    if (newLayer != nullptr) {
        replacement->setLayer(newLayer);
    }
    if (attrs.hasPen()) {
        replacement->setPen(attrs.applyTo(original->getPen(false)));
    }
    for (const QString& note : std::as_const(notes)) {
        warnings.append(note);
    }
    if (newLayer != nullptr && newLayer->isFrozen()) {
        warnings.append(QStringLiteral("layer_frozen: layer '%1' is frozen, so the entity is hidden").arg(newLayer->getName()));
    }

    const bool wasSelected = original->isSelected();
    RS_Entity* added = replacement.release();
    m_document->undoableModify(
        m_viewport,
        [&](LC_DocumentModificationBatch& ctx) {
            ctx.dontSetActiveLayerAndPen();
            ctx.replace(original, added);
            return true;
        },
        [&](LC_DocumentModificationBatch&, RS_Document* doc) {
            if (wasSelected) {
                doc->select(QList<RS_Entity*>{added});
            }
        });
    audit = QStringLiteral("[MCP] changed %1 %2").arg(json::typeName(added)).arg(added->lineage());
    return {{QStringLiteral("entity"), json::entityToJson(added, json::Detail::Full)}, {QStringLiteral("undoable"), true}};
}

QJsonObject LC_RemoteApiEntityOps::remove(const QList<unsigned long long>& ids, QString& audit) const {
    const LC_RemoteApiEntityIndex index(m_document);
    const QList<RS_Entity*> entities = index.requireAll(ids);
    refuseLocked(entities);
    QJsonArray deleted = idsOf(entities);
    m_document->undoableModify(m_viewport, [&](LC_DocumentModificationBatch& ctx) {
        ctx.remove(entities);
        return !entities.isEmpty();
    });
    audit = QStringLiteral("[MCP] deleted ") + countText(static_cast<int>(entities.size()), "entity", "entities");
    return {{QStringLiteral("deleted"), deleted}, {QStringLiteral("undoable"), true}};
}

QJsonObject LC_RemoteApiEntityOps::transform(const LC_JsonReader& body, QString& audit) const {
    body.allowOnly({"ids", "op", "offset", "center", "angle_deg", "base", "factor", "axis", "copy", "copies", "select"});
    const QJsonObject raw = body.raw();
    if (!raw.contains(QStringLiteral("ids"))) {
        json::throwMissingParam(QStringLiteral("ids"));
    }
    const QList<unsigned long long> ids = json::parseIds(raw.value(QStringLiteral("ids")), QStringLiteral("ids"), MaxBatch);
    if (!raw.contains(QStringLiteral("op"))) {
        json::throwMissingParam(QStringLiteral("op"));
    }
    TransformSpec spec;
    spec.op = body.enumOf("op", {QStringLiteral("move"), QStringLiteral("rotate"), QStringLiteral("scale"), QStringLiteral("mirror")},
                          QStringLiteral("move"));
    const QMap<QString, QStringList> opFields{{QStringLiteral("move"), {QStringLiteral("offset")}},
                                              {QStringLiteral("rotate"), {QStringLiteral("center"), QStringLiteral("angle_deg")}},
                                              {QStringLiteral("scale"), {QStringLiteral("base"), QStringLiteral("factor")}},
                                              {QStringLiteral("mirror"), {QStringLiteral("axis")}}};
    for (auto it = opFields.cbegin(); it != opFields.cend(); ++it) {
        if (it.key() == spec.op) {
            continue;
        }
        for (const QString& key : it.value()) {
            if (raw.contains(key)) {
                json::throwBadParam(key, QStringLiteral("'%1' does not apply to op %2 (it takes %3)")
                                             .arg(key, spec.op, opFields.value(spec.op).join(QStringLiteral(", "))));
            }
        }
    }
    if (spec.op == QLatin1String("move")) {
        spec.offset = body.point("offset");
    }
    else if (spec.op == QLatin1String("rotate")) {
        spec.center = body.point("center");
        spec.angle = RS_Math::deg2rad(body.num("angle_deg"));
    }
    else if (spec.op == QLatin1String("scale")) {
        spec.base = body.point("base");
        const QJsonValue factor = raw.value(QStringLiteral("factor"));
        if (factor.isUndefined()) {
            json::throwMissingParam(QStringLiteral("factor"));
        }
        if (factor.isArray()) {
            spec.factor = json::parsePoint(factor, QStringLiteral("factor"));
        }
        else {
            const double f = json::parseNumber(factor, QStringLiteral("factor"));
            spec.factor = RS_Vector(f, f);
        }
        if (std::abs(spec.factor.x) < RS_TOLERANCE || std::abs(spec.factor.y) < RS_TOLERANCE) {
            json::throwBadParam(QStringLiteral("factor"), QStringLiteral("'factor' must not be zero"));
        }
        if (spec.factor.x < 0.0 || spec.factor.y < 0.0) {
            // a negative factor mirrors: text, dimensions and the like would read back with
            // negative sizes; op mirror does it properly
            json::throwBadParam(QStringLiteral("factor"),
                                QStringLiteral("'factor' must be > 0; to flip entities use op mirror"));
        }
    }
    else {
        const QJsonValue axis = raw.value(QStringLiteral("axis"));
        if (axis.isUndefined()) {
            json::throwMissingParam(QStringLiteral("axis"));
        }
        if (!axis.isArray() || axis.toArray().size() != 2) {
            json::throwBadParam(QStringLiteral("axis"), QStringLiteral("'axis' must be two points [[x1, y1], [x2, y2]]"));
        }
        spec.axis1 = json::parsePoint(axis.toArray().at(0), QStringLiteral("axis[0]"));
        spec.axis2 = json::parsePoint(axis.toArray().at(1), QStringLiteral("axis[1]"));
        if (spec.axis1.distanceTo(spec.axis2) < RS_TOLERANCE) {
            json::throwBadParam(QStringLiteral("axis"), QStringLiteral("the two points of 'axis' must differ"));
        }
    }
    const bool copy = body.flag("copy", false);
    const int copies = body.integer("copies", 1, 1, MaxCopies);
    if (copies != 1 && !copy) {
        json::throwBadParam(QStringLiteral("copies"), QStringLiteral("'copies' needs copy: true"));
    }
    if (copies != 1 && spec.op == QLatin1String("mirror")) {
        json::throwBadParam(QStringLiteral("copies"), QStringLiteral("a mirror makes one copy"));
    }
    const bool select = body.flag("select", false);

    const LC_RemoteApiEntityIndex index(m_document);
    const QList<RS_Entity*> entities = index.requireAll(ids);
    refuseLocked(entities);

    if (spec.op == QLatin1String("scale") && !spec.isotropic()) {
        QJsonObject refused;
        for (RS_Entity* e : entities) {
            const QString why = nonUniformScaleRefusal(e);
            if (!why.isEmpty()) {
                refused.insert(QString::number(e->lineage()), why);
            }
        }
        if (!refused.isEmpty()) {
            throw LC_ApiError(422, QStringLiteral("invalid_entity"),
                              QStringLiteral("some entities cannot be scaled by different x and y factors; nothing was changed"),
                              {{QStringLiteral("errors"), refused}});
        }
    }

    // build every result before changing anything
    std::vector<std::unique_ptr<RS_Entity>> results;
    QList<RS_Entity*> replaced;
    QJsonObject failures;
    for (RS_Entity* e : entities) {
        std::unique_ptr<RS_Entity> temporary;
        RS_Entity* source = transformSource(e, spec, temporary);
        for (int n = 1; n <= copies; ++n) {
            std::unique_ptr<RS_Entity> result(source->clone());
            spec.apply(result.get(), n);
            if (result->rtti() == RS2::EntityInsert && !static_cast<RS_Insert*>(result.get())->lastSourceEditSucceeded()) {
                failures.insert(QString::number(e->lineage()), QStringLiteral("LibreCAD cannot apply this %1 to the block reference").arg(spec.op));
                break;
            }
            if (copy) {
                renewCopyLineage(result.get());
            }
            results.push_back(std::move(result));
        }
        if (!copy) {
            replaced.append(e);
        }
    }
    if (!failures.isEmpty()) {
        throw LC_ApiError(422, QStringLiteral("invalid_entity"), QStringLiteral("the transform cannot apply to some entities; nothing was changed"),
                          {{QStringLiteral("errors"), failures}});
    }

    QSet<unsigned long long> selectedLineages;
    for (const RS_Entity* e : entities) {
        if (e->isSelected()) {
            selectedLineages.insert(e->lineage());
        }
    }
    QList<RS_Entity*> added;
    m_document->undoableModify(
        m_viewport,
        [&](LC_DocumentModificationBatch& ctx) {
            ctx.dontSetActiveLayerAndPen();
            ctx.remove(replaced);
            for (auto& result : results) {
                added.append(result.release());
            }
            ctx += added;
            return true;
        },
        [&](LC_DocumentModificationBatch&, RS_Document* doc) {
            QList<RS_Entity*> toSelect;
            for (RS_Entity* e : std::as_const(added)) {
                if (select || (!copy && selectedLineages.contains(e->lineage()))) {
                    toSelect.append(e);
                }
            }
            doc->select(toSelect);
        });

    const QString verb = spec.op == QLatin1String("move")     ? QStringLiteral("moved")
                         : spec.op == QLatin1String("rotate") ? QStringLiteral("rotated")
                         : spec.op == QLatin1String("scale")  ? QStringLiteral("scaled")
                                                              : QStringLiteral("mirrored");
    audit = QStringLiteral("[MCP] %1 %2").arg(verb, countText(static_cast<int>(entities.size()), "entity", "entities"));
    if (copy) {
        audit += QStringLiteral(" as %1").arg(countText(static_cast<int>(added.size()), "copy", "copies"));
    }
    return {{QStringLiteral("entities"), entitiesJson(added, json::Detail::Full)},
            {QStringLiteral("ids"), idsOf(added)},
            {QStringLiteral("originals_kept"), copy},
            {QStringLiteral("undoable"), true}};
}

QJsonObject LC_RemoteApiEntityOps::selection() const {
    const LC_RemoteApiEntityIndex index(m_document);
    QList<RS_Entity*> selected;
    for (RS_Entity* e : index.entities()) {
        if (e->isSelected()) {
            selected.append(e);
        }
    }
    return {{QStringLiteral("ids"), idsOf(selected)}, {QStringLiteral("count"), static_cast<int>(selected.size())}};
}

QJsonObject LC_RemoteApiEntityOps::select(const LC_JsonReader& body) const {
    body.allowOnly({"mode", "ids", "bbox", "bbox_mode", "type", "layer"});
    const QJsonObject raw = body.raw();
    if (!raw.contains(QStringLiteral("mode"))) {
        json::throwMissingParam(QStringLiteral("mode"));
    }
    const QString mode = body.enumOf("mode",
                                     {QStringLiteral("set"), QStringLiteral("add"), QStringLiteral("remove"), QStringLiteral("clear"),
                                      QStringLiteral("all")},
                                     QStringLiteral("set"));
    const LC_RemoteApiEntityFilter filter = LC_RemoteApiEntityFilter::fromBody(raw, m_graphic);
    const bool hasIds = raw.contains(QStringLiteral("ids"));
    const LC_RemoteApiEntityIndex index(m_document);

    if (mode == QLatin1String("clear")) {
        if (hasIds || filter.hasConditions()) {
            json::throwBadParam(QStringLiteral("mode"), QStringLiteral("mode clear takes no ids or filters"));
        }
        RS_Selection::unselectAllInDocument(m_document, m_viewport);
        m_viewport->notifyChanged();
        return selection();
    }

    QList<RS_Entity*> candidates;
    if (hasIds) {
        candidates = index.requireAll(json::parseIds(raw.value(QStringLiteral("ids")), QStringLiteral("ids"), MaxBatch));
    }
    else if (mode == QLatin1String("all") || filter.hasConditions()) {
        candidates = index.entities();
    }
    else {
        json::throwMissingParam(QStringLiteral("ids"));
    }
    QList<RS_Entity*> targets;
    for (RS_Entity* e : std::as_const(candidates)) {
        if (LC_RemoteApiEntityIndex::matches(e, filter)) {
            targets.append(e);
        }
    }

    if (mode == QLatin1String("set")) {
        RS_Selection::unselectAllInDocument(m_document, m_viewport);
    }
    m_document->select(targets, mode != QLatin1String("remove"));
    m_viewport->notifyChanged();
    return selection();
}

QJsonObject LC_RemoteApiEntityOps::measure(const LC_JsonReader& body) const {
    const QJsonObject raw = body.raw();
    if (!raw.contains(QStringLiteral("kind"))) {
        json::throwMissingParam(QStringLiteral("kind"));
    }
    const QString kind = body.enumOf("kind",
                                     {QStringLiteral("distance"), QStringLiteral("length"), QStringLiteral("area"), QStringLiteral("angle"),
                                      QStringLiteral("intersections")},
                                     QStringLiteral("distance"));
    const LC_RemoteApiEntityIndex index(m_document);
    const auto requireIds = [&raw, &index](const int exactCount) {
        if (!raw.contains(QStringLiteral("ids"))) {
            json::throwMissingParam(QStringLiteral("ids"));
        }
        const QList<unsigned long long> ids = json::parseIds(raw.value(QStringLiteral("ids")), QStringLiteral("ids"), MaxBatch);
        if (exactCount > 0 && ids.size() != exactCount) {
            json::throwBadParam(QStringLiteral("ids"), QStringLiteral("'ids' must name exactly %1 different entities").arg(exactCount));
        }
        return index.requireAll(ids);
    };

    if (kind == QLatin1String("distance")) {
        body.allowOnly({"kind", "from", "to", "id"});
        const RS_Vector from = body.point("from");
        if (raw.contains(QStringLiteral("to")) == raw.contains(QStringLiteral("id"))) {
            json::throwBadParam(QStringLiteral("to"), QStringLiteral("give either 'to' (a point) or 'id' (an entity)"));
        }
        if (raw.contains(QStringLiteral("to"))) {
            const RS_Vector to = body.point("to");
            const RS_Vector d = to - from;
            return {{QStringLiteral("distance"), d.magnitude()},
                    {QStringLiteral("dx"), d.x},
                    {QStringLiteral("dy"), d.y},
                    {QStringLiteral("angle_deg"), d.magnitude() > 0.0 ? RS_Math::rad2deg(RS_Math::correctAngle(d.angle())) : 0.0}};
        }
        RS_Entity* e = index.require(json::parseId(raw.value(QStringLiteral("id")), QStringLiteral("id")));
        double distance = RS_MAXDOUBLE;
        const RS_Vector nearest = e->getNearestPointOnEntity(from, true, &distance);
        if (!nearest.valid) {
            json::throwBadParam(QStringLiteral("id"), QStringLiteral("LibreCAD cannot measure a distance to this entity"));
        }
        return {{QStringLiteral("distance"), distance}, {QStringLiteral("nearest_point"), json::pointValue(nearest)}};
    }

    if (kind == QLatin1String("length")) {
        body.allowOnly({"kind", "ids"});
        const QList<RS_Entity*> entities = requireIds(0);
        double total = 0.0;
        QJsonObject perEntity;
        QJsonArray noLength;
        for (RS_Entity* e : entities) {
            if (!hasLength(e)) {
                noLength.append(json::idValue(e));
                continue;
            }
            const double length = e->rtti() == RS2::EntityPolyline ? json::polylineLength(static_cast<RS_Polyline*>(e))
                                                                   : e->getLength();
            if (length < 0.0) {
                noLength.append(json::idValue(e));
                continue;
            }
            total += length;
            perEntity.insert(QString::number(e->lineage()), length);
        }
        if (!noLength.isEmpty()) {
            throw LC_ApiError(400, QStringLiteral("bad_param"),
                              QStringLiteral("some entities have no length: only lines, arcs, circles, ellipses, polylines and "
                                             "splines have one (not text, hatches, points, inserts, dimensions, ...)"),
                              {{QStringLiteral("param"), QStringLiteral("ids")}, {QStringLiteral("ids"), noLength}});
        }
        return {{QStringLiteral("length"), total}, {QStringLiteral("per_entity"), perEntity}};
    }

    if (kind == QLatin1String("area")) {
        body.allowOnly({"kind", "ids", "points"});
        if (raw.contains(QStringLiteral("ids")) == raw.contains(QStringLiteral("points"))) {
            json::throwBadParam(QStringLiteral("ids"), QStringLiteral("give either 'ids' (closed entities) or 'points' (a polygon)"));
        }
        if (raw.contains(QStringLiteral("points"))) {
            const QList<RS_Vector> points = json::parsePoints(raw.value(QStringLiteral("points")), QStringLiteral("points"), 3);
            double twice = 0.0;
            double perimeter = 0.0;
            for (int i = 0; i < points.size(); ++i) {
                const RS_Vector& a = points.at(i);
                const RS_Vector& b = points.at((i + 1) % points.size());
                twice += a.x * b.y - b.x * a.y;
                perimeter += a.distanceTo(b);
            }
            return {{QStringLiteral("area"), std::abs(twice) / 2.0}, {QStringLiteral("perimeter"), perimeter}};
        }
        const QList<RS_Entity*> entities = requireIds(0);
        double totalArea = 0.0;
        double totalPerimeter = 0.0;
        QJsonObject perEntity;
        QJsonArray open;
        for (RS_Entity* e : entities) {
            double area = 0.0;
            double perimeter = 0.0;
            if (!closedArea(e, area, perimeter)) {
                open.append(json::idValue(e));
                continue;
            }
            totalArea += area;
            totalPerimeter += perimeter;
            perEntity.insert(QString::number(e->lineage()),
                             QJsonObject{{QStringLiteral("area"), area}, {QStringLiteral("perimeter"), perimeter}});
        }
        if (!open.isEmpty()) {
            throw LC_ApiError(422, QStringLiteral("not_closed"),
                              QStringLiteral("only closed polylines, circles, full ellipses, closed splines and hatches have an area"),
                              {{QStringLiteral("ids"), open}});
        }
        return {{QStringLiteral("area"), totalArea}, {QStringLiteral("perimeter"), totalPerimeter}, {QStringLiteral("per_entity"), perEntity}};
    }

    if (kind == QLatin1String("angle")) {
        body.allowOnly({"kind", "ids", "points"});
        if (raw.contains(QStringLiteral("ids")) == raw.contains(QStringLiteral("points"))) {
            json::throwBadParam(QStringLiteral("ids"), QStringLiteral("give either 'ids' (two lines) or 'points' ([vertex, p1, p2])"));
        }
        if (raw.contains(QStringLiteral("points"))) {
            const QList<RS_Vector> points = json::parsePoints(raw.value(QStringLiteral("points")), QStringLiteral("points"), 3);
            if (points.size() != 3) {
                json::throwBadParam(QStringLiteral("points"), QStringLiteral("'points' must be [vertex, p1, p2]"));
            }
            const RS_Vector d1 = points.at(1) - points.at(0);
            const RS_Vector d2 = points.at(2) - points.at(0);
            if (d1.magnitude() < RS_TOLERANCE || d2.magnitude() < RS_TOLERANCE) {
                json::throwBadParam(QStringLiteral("points"), QStringLiteral("p1 and p2 must differ from the vertex"));
            }
            return {{QStringLiteral("angle_deg"), angleBetween(d1, d2)}};
        }
        const QList<RS_Entity*> lines = requireIds(2);
        QJsonArray notLines;
        for (const RS_Entity* e : lines) {
            if (e->rtti() != RS2::EntityLine) {
                notLines.append(json::idValue(e));
            }
        }
        if (!notLines.isEmpty()) {
            throw LC_ApiError(400, QStringLiteral("bad_param"), QStringLiteral("the angle between entities needs two lines"),
                              {{QStringLiteral("param"), QStringLiteral("ids")}, {QStringLiteral("ids"), notLines}});
        }
        const auto direction = [](const RS_Entity* e) {
            return e->getEndpoint() - e->getStartpoint();
        };
        return {{QStringLiteral("angle_deg"), angleBetween(direction(lines.at(0)), direction(lines.at(1)))}};
    }

    // intersections
    body.allowOnly({"kind", "ids"});
    const QList<RS_Entity*> pair = requireIds(2);
    QList<RS_Vector> found;
    for (RS_Entity* a : atomicPieces(pair.at(0))) {
        for (RS_Entity* b : atomicPieces(pair.at(1))) {
            const RS_VectorSolutions solutions = RS_Information::getIntersection(a, b, true);
            for (const RS_Vector& p : solutions) {
                if (!p.valid) {
                    continue;
                }
                const bool known = std::any_of(found.cbegin(), found.cend(), [&p](const RS_Vector& q) {
                    return q.distanceTo(p) < 1e-9;
                });
                if (!known) {
                    found.append(p);
                }
            }
        }
    }
    return {{QStringLiteral("points"), json::pointsValue(found)}};
}
