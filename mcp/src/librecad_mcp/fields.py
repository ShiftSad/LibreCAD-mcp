# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared parameter descriptions and the entity spec models of add_entities.

Field names are exactly the Remote API's (DESIGN.md section 2.6): what a tool sends is what
LibreCAD reads, and what query_entities returns uses the same names. Coordinates are drawing
units, angles are degrees (counter-clockwise from +X), lineweights are millimetres.
"""
from __future__ import annotations

from typing import Annotated, Literal, Union

from pydantic import BaseModel, ConfigDict, Field, model_validator

# --- scalar and point types -----------------------------------------------------------------

Num = Annotated[float, Field(allow_inf_nan=False)]
PositiveNum = Annotated[float, Field(gt=0, allow_inf_nan=False)]
Point = Annotated[list[Num], Field(min_length=2, max_length=2)]
Vertex = Annotated[list[Num], Field(
    min_length=2, max_length=3,
    description="[x, y] or [x, y, bulge]. bulge = tan(angle/4) of an arc segment to the next "
                "vertex (positive = counter-clockwise, 0 or omitted = straight)")]
Color = Union[
    Annotated[str, Field(pattern=r"^(ByLayer|ByBlock|#[0-9A-Fa-f]{6})$",
                         description='"ByLayer", "ByBlock" or "#rrggbb"')],
    Annotated[int, Field(ge=1, le=255, description="AutoCAD color index (ACI) 1..255")],
]
LayerColor = Union[
    Annotated[str, Field(pattern=r"^#[0-9A-Fa-f]{6}$", description='"#rrggbb"')],
    Annotated[int, Field(ge=1, le=255, description="AutoCAD color index (ACI) 1..255")],
]
Lineweight = Union[
    Literal["ByLayer", "ByBlock", "Default"],
    Annotated[float, Field(ge=0, le=2.11, allow_inf_nan=False,
                           description="Millimetres, one of the standard widths (0.00, 0.05, 0.09, "
                                       "0.13, 0.15, 0.18, 0.20, 0.25, 0.30, 0.35, 0.40, 0.50, 0.53, "
                                       "0.60, 0.70, 0.80, 0.90, 1.00, 1.06, 1.20, 1.40, 1.58, 2.00, 2.11)")],
]

EntityTypeName = Literal["line", "circle", "arc", "ellipse", "point", "polyline", "spline", "text",
                         "mtext", "dimension", "hatch", "insert", "image", "leader", "mleader",
                         "solid", "parabola", "hyperbola", "other"]

# --- shared parameter descriptions ----------------------------------------------------------

_DOC = Field(description="Id of the drawing, from list_documents ('id'). Omit for the active drawing.")
_ENTITY_ID = Field(description="Integer id of the entity, from the 'id' field of query_entities or "
                               "add_entities. Ids survive edits and undo; copies get new ids.")
_ENTITY_IDS = Field(description="Integer entity ids, from the 'id' field of query_entities or add_entities.")
_POINT = Field(description="[x, y] in drawing units.")
_LAYER_NAME = Field(description="Layer name, exactly as list_layers shows it (case-sensitive).")
_LAYER_COLOR = Field(description='Layer color: "#rrggbb" or an AutoCAD color index 1..255 (1 red, '
                                 '2 yellow, 3 green, 4 cyan, 5 blue, 6 magenta, 7 white/black, 8 grey).')
_COLOR = Field(description='Entity color: "ByLayer", "ByBlock", "#rrggbb" or an AutoCAD color index 1..255.')
_LINETYPE = Field(description='Linetype name, e.g. "Continuous", "Dashed", "DashDot", "Center", '
                              '"Hidden", "Dot", or "ByLayer"/"ByBlock" for entities.')
_LINEWEIGHT_MM = Field(description='Lineweight in millimetres (0.25, 0.35, 0.5, ...; snapped to the '
                                   'standard width), or "ByLayer"/"ByBlock"/"Default".')
_TYPES = Field(description="Only these entity types (any of them).")
_LAYERS = Field(description="Only entities on these layers (any of them); exact names.")
_BBOX_MIN = Field(description="Lower-left corner [x, y] of an area filter; give bbox_max too.")
_BBOX_MAX = Field(description="Upper-right corner [x, y] of an area filter; give bbox_min too.")
_BBOX_MODE = Field(description='"crossing": entities touching the area; "window": entities fully inside it.')


# --- entity specs (add_entities) ----------------------------------------------------------------

def _slim_schema(schema: dict, cls=None) -> None:
    """Keep the add_entities schema small (it repeats per entity type): drop pydantic's titles and
    show an optional field as its plain type; omitting it means null anyway."""
    schema.pop("title", None)
    for prop in (schema.get("properties") or {}).values():
        prop.pop("title", None)
        variants = prop.get("anyOf")
        if variants and "default" in prop and prop["default"] is None:
            rest = [v for v in variants if v != {"type": "null"}]
            if len(rest) < len(variants):
                del prop["default"]
                if len(rest) == 1:
                    del prop["anyOf"]
                    for key, value in rest[0].items():
                        prop.setdefault(key, value)
                else:
                    prop["anyOf"] = rest


_SpecColor = Union[Annotated[str, Field(pattern=r"^(ByLayer|ByBlock|#[0-9A-Fa-f]{6})$")],
                   Annotated[int, Field(ge=1, le=255)]]
_SpecLineweight = Union[Literal["ByLayer", "ByBlock", "Default"],
                        Annotated[float, Field(ge=0, le=2.11, allow_inf_nan=False)]]


class _Attrs(BaseModel):
    model_config = ConfigDict(extra="forbid", json_schema_extra=_slim_schema)

    layer: str | None = Field(None, description="Default: the call's layer, else the current layer.")
    color: _SpecColor | None = Field(None, description='"ByLayer" (default), "ByBlock", "#rrggbb" or ACI 1..255.')
    linetype: str | None = Field(None, description='Default "ByLayer".')
    lineweight: _SpecLineweight | None = Field(None, description='Millimetres, or "ByLayer" (default).')


class LineSpec(_Attrs):
    """A straight line from start to end."""
    type: Literal["line"]
    start: Point
    end: Point


class CircleSpec(_Attrs):
    """A full circle."""
    type: Literal["circle"]
    center: Point
    radius: PositiveNum


class ArcSpec(_Attrs):
    """A circular arc, counter-clockwise from start_angle_deg to end_angle_deg (clockwise when reversed)."""
    type: Literal["arc"]
    center: Point
    radius: PositiveNum
    start_angle_deg: Num
    end_angle_deg: Num
    reversed: bool = False


class EllipseSpec(_Attrs):
    """An ellipse or elliptic arc. major_axis is the vector from the center to the end of the major
    axis; ratio is minor/major in (0, 1]."""
    type: Literal["ellipse"]
    center: Point
    major_axis: Point
    ratio: Annotated[float, Field(gt=0, le=1, allow_inf_nan=False)]
    start_angle_deg: Num = 0.0
    end_angle_deg: Num = 360.0
    reversed: bool = False


class PointSpec(_Attrs):
    """A point entity."""
    type: Literal["point"]
    position: Point


class PolylineSpec(_Attrs):
    """A polyline through vertices (at least 2); closed joins the last vertex to the first."""
    type: Literal["polyline"]
    vertices: Annotated[list[Vertex], Field(min_length=2)]
    closed: bool = False


class SplineSpec(_Attrs):
    """A spline: EITHER control_points (at least degree+1) with degree 1..3, OR fit_points (at
    least 2) that the curve passes through."""
    type: Literal["spline"]
    control_points: list[Point] | None = None
    degree: Annotated[int, Field(ge=1, le=3)] = 3
    fit_points: list[Point] | None = None
    closed: bool = False

    @model_validator(mode="after")
    def _one_kind(self):
        if (self.control_points is None) == (self.fit_points is None):
            raise ValueError("give either control_points or fit_points, not both or neither")
        if self.control_points is not None and len(self.control_points) < self.degree + 1:
            raise ValueError(f"a degree {self.degree} spline needs at least {self.degree + 1} control_points")
        if self.fit_points is not None:
            if len(self.fit_points) < 2:
                raise ValueError("fit_points needs at least 2 points")
            if "degree" in self.model_fields_set:
                raise ValueError("degree applies to control_points splines only")
        return self


class TextSpec(_Attrs):
    """Single-line text. position is the alignment point."""
    type: Literal["text"]
    position: Point
    text: Annotated[str, Field(min_length=1)]
    height: PositiveNum
    angle_deg: Num = 0.0
    halign: Literal["left", "center", "right", "aligned", "middle", "fit"] = "left"
    valign: Literal["baseline", "bottom", "middle", "top"] = "baseline"
    style: str = "standard"
    width_factor: PositiveNum = 1.0


class MTextSpec(_Attrs):
    """Multi-line text; width 0 means no wrapping. Use \\P for a line break inside text."""
    type: Literal["mtext"]
    position: Point
    text: Annotated[str, Field(min_length=1)]
    height: PositiveNum
    width: Annotated[float, Field(ge=0, allow_inf_nan=False)] = 0.0
    angle_deg: Num = 0.0
    attachment: Literal["top_left", "top_center", "top_right", "middle_left", "middle_center",
                        "middle_right", "bottom_left", "bottom_center", "bottom_right"] = "top_left"
    line_spacing: PositiveNum = 1.0
    style: str = "standard"


_DIM_FIELDS = {
    "linear": ({"p1", "p2", "line_point"}, {"angle_deg"}),
    "aligned": ({"p1", "p2", "line_point"}, set()),
    "radial": ({"center", "point"}, {"leader_length"}),
    "diametric": ({"point", "opposite_point"}, set()),
    "angular": ({"line1", "line2", "arc_point"}, set()),
}
_DIM_GEOMETRY = {"p1", "p2", "line_point", "angle_deg", "center", "point", "leader_length",
                 "opposite_point", "line1", "line2", "arc_point"}


class DimensionSpec(_Attrs):
    """A dimension. dim_type "linear": p1, p2, line_point, angle_deg (0 horizontal, 90 vertical);
    "aligned": p1, p2, line_point; "radial": center, point, leader_length; "diametric": point,
    opposite_point; "angular": line1, line2 (each [[x1, y1], [x2, y2]]), arc_point. line_point
    and arc_point place the dimension line or arc."""
    type: Literal["dimension"]
    dim_type: Literal["linear", "aligned", "radial", "diametric", "angular"]
    p1: Point | None = None
    p2: Point | None = None
    line_point: Point | None = None
    angle_deg: Num | None = None
    center: Point | None = None
    point: Point | None = None
    leader_length: Annotated[float, Field(ge=0, allow_inf_nan=False)] | None = None
    opposite_point: Point | None = None
    line1: Annotated[list[Point], Field(min_length=2, max_length=2)] | None = None
    line2: Annotated[list[Point], Field(min_length=2, max_length=2)] | None = None
    arc_point: Point | None = None
    text: str = Field("", description='Override text; "" shows the measured value.')
    style: str | None = Field(None, description="Dimension style; default: the drawing's default.")

    @model_validator(mode="after")
    def _fields_of_dim_type(self):
        required, optional = _DIM_FIELDS[self.dim_type]
        given = {f for f in _DIM_GEOMETRY if getattr(self, f) is not None}
        missing = sorted(required - given)
        if missing:
            raise ValueError(f"a {self.dim_type} dimension needs {', '.join(missing)}")
        extra = sorted(given - required - optional)
        if extra:
            raise ValueError(f"{', '.join(extra)} do not apply to a {self.dim_type} dimension")
        return self


class HatchLoop(BaseModel):
    """One boundary loop of a hatch."""
    model_config = ConfigDict(extra="forbid", json_schema_extra=_slim_schema)
    vertices: Annotated[list[Vertex], Field(min_length=2)]
    closed: bool = True


class HatchSpec(_Attrs):
    """A hatch (pattern fill). Boundary EITHER inline (boundary: loops of [x, y] points or
    {vertices, closed}) OR the ids of existing closed entities (boundary_ids)."""
    type: Literal["hatch"]
    pattern: str = Field("SOLID", description='"SOLID" or a pattern name such as "ANSI31".')
    scale: PositiveNum = 1.0
    angle_deg: Num = 0.0
    boundary: list[Union[Annotated[list[Point], Field(min_length=3)], HatchLoop]] | None = None
    boundary_ids: list[int] | None = None

    @model_validator(mode="after")
    def _one_boundary(self):
        if (self.boundary is None) == (self.boundary_ids is None):
            raise ValueError("give either boundary or boundary_ids, not both or neither")
        if self.boundary is not None and not self.boundary:
            raise ValueError("boundary needs at least one loop")
        if self.boundary_ids is not None and not self.boundary_ids:
            raise ValueError("boundary_ids needs at least one id")
        return self


class InsertSpec(_Attrs):
    """A block reference (insert) of an existing block definition (list_blocks)."""
    type: Literal["insert"]
    block: Annotated[str, Field(min_length=1)]
    position: Point
    scale: Union[PositiveNum, Annotated[list[PositiveNum], Field(min_length=2, max_length=2)]] = 1.0
    angle_deg: Num = 0.0
    columns: Annotated[int, Field(ge=1, le=1000)] = 1
    rows: Annotated[int, Field(ge=1, le=1000)] = 1
    spacing: Point = Field(default_factory=lambda: [0.0, 0.0])


EntitySpec = Annotated[
    Union[LineSpec, CircleSpec, ArcSpec, EllipseSpec, PointSpec, PolylineSpec, SplineSpec,
          TextSpec, MTextSpec, DimensionSpec, HatchSpec, InsertSpec],
    Field(discriminator="type"),
]


def spec_to_json(spec: BaseModel) -> dict:
    """The wire form of a spec: only the fields the caller set (LibreCAD applies the defaults)."""
    return spec.model_dump(mode="json", exclude_unset=True, exclude_none=True)
