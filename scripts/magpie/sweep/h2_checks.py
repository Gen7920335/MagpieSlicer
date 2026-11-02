#!/usr/bin/env python3
"""G-code checks for H/2 (half-height) support and its range (half_layer.support_scope).

Every check reads the G-code alone (;TYPE, ;HEIGHT, ;WIDTH, Z and E of the moves, and the config
block the slicer writes at its end) and the case's own input settings; none reads a planner number.
Distances and heights are in mm. An issue is a dict: check, z, xy, measured, message.

Checks of one G-code:
  integrity      the G-code ends with its executable and config blocks and extrudes something
  contact_top    support under the model, within the contact window, prints in bands
  contact_bottom support standing on the model, within the window, prints in bands
  all_banded     range "all": every support path prints in a band
  deep_body      range "contact": some of the deep support body prints at a whole layer
  island         range "contact": a banded support island has the model within reach of the window, or
                 an interface band beside it in its own Z (it is banded with that band)
  first_layer    range "contact": a support body island on the bed keeps the whole first layer
  overlap        two support paths of different Z overlap in Z at the same XY (double extrusion)
  gap            the support around an XY has a band-sized Z hole
  flow           a support path's extrusion per mm matches its width and height
  z_safety       the nozzle never runs below material printed earlier at the same XY
  tower_height   a tower (prime tower, temperature drop tower) layer is as tall as its rise from the
                 tower's previous layer (H/2 bands made the drop tower print half its rise)
Checks of a "contact" G-code against the same case sliced with "all" and with H/2 off:
  coverage       the support's XY footprint at each Z is the same as both
  model          the object's paths are the same as "all" (the range changes the support only)
  time           "contact" takes no longer than "all"

A band is at most half of the model layer (the configured layer height; the object's first layer
at and under it). "Banded" in island and first_layer means thinner than the H/2-off slice's own
support layer at that XY: support layers can be thin without H/2 (a filler under a contact layer,
resin support), so a thin path alone proves nothing. A raft is never banded and is left out.

The contact window follows the feature's definition (Gen 2026-11-01): the support's Z gap plus 1.5
layers without interface layers on that side (the contact layer and the next), plus 0.5 layers
with them. An island is judged whole, so a path of a banded island may lie away from the model.
"""
import bisect
import math
import re

# Tolerances, mm.
HEIGHT_TOL_MM = 0.005        # ;HEIGHT is printed to 3 decimals; a band is at most half a model layer
WINDOW_TOL_MM = 0.005        # a support path this far inside the contact window must be banded
CELL_MM = 0.25               # fine XY raster: path centrelines; finer than the support XY gap
SAMPLE_MM = 0.125            # spacing of the points sampled along a path (half a cell)
DEEP_EXTRA_LAYERS = 2.0      # deep body: no model within the window plus this many layers
DEEP_MIN_MM = 10.0           # deep body path length from which some of it must be whole-height
WHOLE_SHARE = 0.9            # a whole-height path is at least this share of the layer height
OVERLAP_MM = 0.02            # Z overlap of two support paths at one XY that counts as double extrusion
MIN_VOLUME_MM3 = 0.001        # paths extruding less material are slivers for overlap (a 0.03 mm wide interface remnant)
BAND_TOL_MM = 0.01           # a path this much thinner than the H/2-off layer at its XY is banded
GAP_MIN_MM = 0.05            # a hole in a support column shorter than this is rounding
GAP_CELL_MM = 2.0            # coarse raster for support columns; finer than the support pattern spacing
FLOW_LOW, FLOW_HIGH = 0.8, 1.25  # extruded volume / (width x height bead) of a support path
FLOW_MIN_LEN_MM = 1.0        # shorter paths carry E rounded to 5 decimals
TOWER_TOL_MM = 0.003          # tower bead height against its Z rise (both printed to 0.001 mm)
Z_SAFETY_MM = 0.02           # material this far above a path's bottom, or a travel's nozzle, collides
# Footprint change per Z bin that counts, mm2, in cells one support line pitch wide (~9 mm2). Against
# "all" correct builds differ by 0 (102 cases of seed 31, 2026-11-02); against H/2 off by up to 117 mm2, 3.6 %
# (hilda03: a 0.1 mm interface band fills at its own line pitch, the lines at the edges move; every band holds the
# whole layer's polygons, probe 2026-11-02). An island smaller than the threshold can vanish unseen.
COVER_MIN_MM2 = 20.0
COVER_OFF_MIN_MM2 = 60.0
COVER_OFF_SHARE = 0.05       # ... and this share of the footprint, against H/2 off
COVER_Z_MM = 0.002           # Z overlap with a bin that counts (Z is printed to 0.001 mm)
TIME_SHARE = 0.01            # "contact" may take this much longer than "all" (estimator rounding)
REPORT_MAX = 6               # issues listed per check

SUPPORT_TYPES = ('Support', 'Support interface', 'Support transition')
NOT_MODEL_TYPES = SUPPORT_TYPES + ('Skirt', 'Brim', 'Prime tower', 'Wipe tower', 'Custom', 'Undefined', '')
NUM = re.compile(r'([XYZEF])(-?[0-9.]+(?:e-?[0-9]+)?)', re.IGNORECASE)
CONFIG_LINE = re.compile(r'^; (\w+) = (.*)$')
TIME_LINE = re.compile(r'^; (?:estimated printing time \(normal mode\)|model printing time) *[=:] *([^;\n]+)')


class Move:
    __slots__ = ('kind', 'z', 'h', 'w', 'x0', 'y0', 'x1', 'y1', 'e', 'line')

    def __init__(self, kind, z, h, w, x0, y0, x1, y1, e, line):
        self.kind, self.z, self.h, self.w = kind, z, h, w
        self.x0, self.y0, self.x1, self.y1, self.e, self.line = x0, y0, x1, y1, e, line

    @property
    def length(self):
        return math.hypot(self.x1 - self.x0, self.y1 - self.y0)

    def samples(self, step=SAMPLE_MM):
        n = max(1, int(self.length / step))
        for i in range(n + 1):
            t = i / n
            yield self.x0 + t * (self.x1 - self.x0), self.y0 + t * (self.y1 - self.y0)


def cell(x, y, size=CELL_MM):
    return int(math.floor(x / size)), int(math.floor(y / size))


def parse_time(text):
    seconds = 0
    for n, unit in re.findall(r'(\d+)\s*([dhms])', text):
        seconds += int(n) * {'d': 86400, 'h': 3600, 'm': 60, 's': 1}[unit]
    return seconds


class Gcode:
    """The moves of a G-code in print order: extrusions (kind = ;TYPE) and travels (kind None)."""

    def __init__(self, path):
        self.path = path
        self.moves = []
        self.config = {}
        self.time_s = None
        self.markers = set()
        x = y = z = h = 0.0
        w = 0.4
        kind = ''
        relative_e = False
        relative_xyz = False   # G91: X, Y and Z relative (tool change macros)
        last_e = 0.0
        with open(path, encoding='utf-8', errors='replace') as f:
            for n, raw in enumerate(f, 1):
                if raw.startswith(';'):
                    if raw.startswith(';TYPE:'):
                        kind = raw[6:].strip()
                    elif raw.startswith(';HEIGHT:'):
                        h = float(raw[8:])
                    elif raw.startswith(';WIDTH:'):
                        w = float(raw[7:])
                    elif raw.startswith('; ') and ('_BLOCK_END' in raw):
                        self.markers.add(raw[2:].strip())
                    else:
                        m = CONFIG_LINE.match(raw)
                        if m:
                            self.config[m.group(1)] = m.group(2).strip()
                        m = TIME_LINE.match(raw)
                        if m and self.time_s is None:
                            self.time_s = parse_time(m.group(1))
                    continue
                line = raw.split(';', 1)[0].strip()
                if not line:
                    continue
                word = line.split()[0].upper()
                if word == 'M83':
                    relative_e = True
                    continue
                if word == 'M82':
                    relative_e = False
                    continue
                if word in ('G90', 'G91'):
                    relative_xyz = word == 'G91'
                    continue
                if word == 'G92':
                    for k, v in NUM.findall(line):
                        if k.upper() == 'E':
                            last_e = float(v)
                    continue
                if word not in ('G0', 'G1', 'G2', 'G3'):
                    continue
                values = {k.upper(): float(v) for k, v in NUM.findall(line)}
                if relative_xyz:
                    nx, ny = x + values.get('X', 0.0), y + values.get('Y', 0.0)
                    z += values.get('Z', 0.0)
                else:
                    nx, ny = values.get('X', x), values.get('Y', y)
                    if 'Z' in values:
                        z = values['Z']
                e = 0.0
                if 'E' in values:
                    e = values['E'] if relative_e else values['E'] - last_e
                    if not relative_e:
                        last_e = values['E']
                if nx != x or ny != y:
                    self.moves.append(Move(kind if e > 0 else None, z, h, w, x, y, nx, ny, e, n))
                x, y = nx, ny

    def number(self, key, default=None, index=0):
        value = self.config.get(key)
        if value is None:
            return default
        try:
            return float(value.split(',')[index])
        except ValueError:
            return default

    def extrusions(self):
        return (m for m in self.moves if m.kind is not None)

    def support(self):
        return [m for m in self.moves if m.kind in SUPPORT_TYPES]

    def model(self):
        return [m for m in self.moves if m.kind is not None and m.kind not in NOT_MODEL_TYPES]


def issue(check, message, z=None, xy=None, measured=None):
    return {'check': check, 'z': None if z is None else round(z, 3),
            'xy': None if xy is None else [round(xy[0], 2), round(xy[1], 2)], 'measured': measured, 'message': message}


class Settings:
    """The case's H/2 settings and the slicer's config as the G-code states it, mm."""

    def __init__(self, g, scope):
        self.scope = scope
        self.layer = g.number('layer_height', 0.2)
        self.first = g.number('initial_layer_print_height', self.layer)
        gap_top = g.number('support_top_z_distance', 0.0)
        gap_bottom = g.number('support_bottom_z_distance', 0.0)
        # The slicer takes the top gap where the bottom one is not set.
        self.gap_top, self.gap_bottom = gap_top, gap_bottom if gap_bottom > 0 else gap_top
        top_layers = int(g.number('support_interface_top_layers', 0))
        bottom_layers = int(g.number('support_interface_bottom_layers', -1))
        # -1: the same as the top.
        bottom_layers = top_layers if bottom_layers < 0 else bottom_layers
        self.iface_top, self.iface_bottom = max(top_layers, 0), max(bottom_layers, 0)
        self.window_top = self.gap_top + (1.5 if top_layers <= 0 else 0.5) * self.layer
        self.window_bottom = self.gap_bottom + (1.5 if bottom_layers <= 0 else 0.5) * self.layer
        self.filament_area = math.pi * (g.number('filament_diameter', 1.75) / 2) ** 2
        self.raft = g.number('raft_layers', 0) > 0
        spacing = g.number('support_base_pattern_spacing', 2.5)
        # Reach of a support island's fill: half the line spacing and a line, mm.
        self.island_reach = 0.5 * spacing + 0.5
        # Line pitch of the support pattern, mm: a footprint cell this wide holds a line of any fill.
        self.pattern_pitch = spacing + 0.5


class ModelIndex:
    """Where the object is: per fine XY cell, the sorted bottoms and tops of its paths, and the Z
    intervals of its layers (a whole layer and, with H/2 outer walls, its bands)."""

    def __init__(self, g, only_cells=None, size=CELL_MM):
        self.size = size
        bottoms, tops, intervals = {}, {}, set()
        for m in g.model():
            intervals.add((round(m.z, 4), round(m.h, 4)))
            for x, y in m.samples():
                c = cell(x, y, size)
                if only_cells is not None and c not in only_cells:
                    continue
                bottoms.setdefault(c, set()).add(round(m.z - m.h, 4))
                tops.setdefault(c, set()).add(round(m.z, 4))
        self.bottoms = {c: sorted(v) for c, v in bottoms.items()}
        self.tops = {c: sorted(v) for c, v in tops.items()}
        self.intervals = sorted(intervals)
        self.lowest = min((t - h for t, h in self.intervals), default=0.0)
        self.first_whole = max((h for t, h in self.intervals if t - h < self.lowest + HEIGHT_TOL_MM), default=0.0)

    def above(self, c, z):
        """Distance from z up to the nearest model bottom over cell c."""
        b = self.bottoms.get(c)
        if not b:
            return math.inf
        i = bisect.bisect_left(b, z - HEIGHT_TOL_MM)
        return b[i] - z if i < len(b) else math.inf

    def below(self, c, z):
        """Distance from z down to the nearest model top under cell c."""
        t = self.tops.get(c)
        if not t:
            return math.inf
        i = bisect.bisect_right(t, z + HEIGHT_TOL_MM)
        return z - t[i - 1] if i > 0 else math.inf

    def whole_layer(self, z, layer):
        """The model layer height a support band at z is half of: the object's first layer at and
        under it (an object lifted by resin support starts high, and its first layer is often
        thicker), the configured layer height above."""
        if self.intervals and z <= self.lowest + self.first_whole + HEIGHT_TOL_MM:
            return self.first_whole
        return layer


class Report:
    def __init__(self):
        self.issues = {}
        self.counts = {}
        self.points = {}

    def add(self, item):
        self.counts[item['check']] = self.counts.get(item['check'], 0) + 1
        bucket = self.issues.setdefault(item['check'], [])
        if len(bucket) < REPORT_MAX:
            bucket.append(item)

    def as_dict(self):
        return {'counts': self.counts, 'issues': self.issues, 'points': self.points}


def check_integrity(g, report):
    for marker in ('EXECUTABLE_BLOCK_END', 'CONFIG_BLOCK_END'):
        if marker not in g.markers:
            report.add(issue('integrity', f'G-code truncated: no {marker}'))
    if not any(True for _ in g.extrusions()):
        report.add(issue('integrity', 'G-code extrudes nothing'))


class NaturalIndex:
    """The support's own layers, from the same case sliced with H/2 off: per coarse XY cell, the Z
    intervals [bottom, top] of its support paths. A path of the H/2 G-code is banded where it is
    thinner than the off-slice layer holding its top at that XY."""

    def __init__(self, g, size):
        self.size = size
        self.spans = {}
        for m in g.support():
            for x, y in m.samples():
                self.spans.setdefault(cell(x, y, size), set()).add((round(m.z - m.h, 4), round(m.z, 4)))

    def interval(self, x, y, z):
        """The thinnest off-slice support layer [bottom, top] holding z near (x, y); None where there is none."""
        c = cell(x, y, self.size)
        best = None
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for b, t in self.spans.get((c[0] + dx, c[1] + dy), ()):
                    if b + HEIGHT_TOL_MM < z <= t + HEIGHT_TOL_MM and (best is None or t - b < best[1] - best[0]):
                        best = (b, t)
        return best

    def height(self, x, y, z):
        found = self.interval(x, y, z)
        return None if found is None else found[1] - found[0]

    def banded(self, m):
        natural = self.height(m.x0, m.y0, m.z)
        return None if natural is None else m.h < natural - BAND_TOL_MM


def _windows(model, s, m):
    """(top hit, bottom hit, deep) of a support path. A hit is (distance to the model, x, y) of the
    nearest point with the model within the contact window over (top) or under (bottom) it, else
    None; deep: the model nowhere within the window plus DEEP_EXTRA_LAYERS layers."""
    top_hit = bottom_hit = None
    deep = True
    for x, y in m.samples():
        c = cell(x, y)
        above, below = model.above(c, m.z), model.below(c, m.z - m.h)
        if above < s.window_top - WINDOW_TOL_MM and (top_hit is None or above < top_hit[0]):
            top_hit = (round(above, 3), round(x, 2), round(y, 2))
        if below < s.window_bottom - WINDOW_TOL_MM and (bottom_hit is None or below < bottom_hit[0]):
            bottom_hit = (round(below, 3), round(x, 2), round(y, 2))
        if above <= s.window_top + DEEP_EXTRA_LAYERS * s.layer or below <= s.window_bottom + DEEP_EXTRA_LAYERS * s.layer:
            deep = False
    return top_hit, bottom_hit, deep


def _deep_island(coarse, s, m):
    """No model within the window plus DEEP_EXTRA_LAYERS layers anywhere within an island's reach
    (coarse cell and its neighbours): an island is judged whole, so a path of an island next to the
    model is not deep body even where the model is not right over it."""
    c = cell(m.x0, m.y0, coarse.size)
    top = s.window_top + DEEP_EXTRA_LAYERS * s.layer
    bottom = s.window_bottom + DEEP_EXTRA_LAYERS * s.layer
    return not any(coarse.above((c[0] + dx, c[1] + dy), m.z) <= top or
                   coarse.below((c[0] + dx, c[1] + dy), m.z - m.h) <= bottom
                   for dx in (-2, -1, 0, 1, 2) for dy in (-2, -1, 0, 1, 2))


def _island_reaches_model(coarse, s, x, y, bottom, top, span=1):
    """The model within reach of an island's own support layer [bottom, top] anywhere within `span`
    coarse cells of (x, y): the contact window plus the side's interface layers, each as thick as
    that support layer (interfaces are always banded, and tree support prints them as ;TYPE:Support)."""
    c = cell(x, y, coarse.size)
    thick = max(s.layer, top - bottom)
    reach_top = s.window_top + s.iface_top * thick + WINDOW_TOL_MM
    reach_bottom = s.window_bottom + s.iface_bottom * thick + WINDOW_TOL_MM
    return any(coarse.above((c[0] + dx, c[1] + dy), top) < reach_top or
               coarse.below((c[0] + dx, c[1] + dy), bottom) < reach_bottom
               for dx in range(-span, span + 1) for dy in range(-span, span + 1))


def check_heights(g, s, report, natural=None, geo=None):
    """contact_top, contact_bottom, all_banded, deep_body, first_layer.

    geo: the G-code whose object paths say where the model is (the H/2-off slice when there is one:
    H/2 outer walls move the walls' paths a little, the support was planned on the slices)."""
    support = g.support()
    if not support:
        return
    geo = geo or g
    cells = {cell(x, y) for m in support for x, y in m.samples()}
    model = ModelIndex(geo, cells)
    reach = max(CELL_MM, s.island_reach)
    coarse = ModelIndex(geo, None, reach)
    deep_mm = deep_whole_mm = 0.0
    window_paths = bed_paths = 0
    for m in support:
        if s.raft and m.z < model.lowest - HEIGHT_TOL_MM:
            continue   # the raft and its interface are never banded
        limit = 0.5 * model.whole_layer(m.z, s.layer) + HEIGHT_TOL_MM
        if s.scope == 'all':
            if m.h > limit:
                report.add(issue('all_banded', 'support path at a whole layer under range "all"', m.z, (m.x0, m.y0),
                                 {'height': m.h, 'band_limit': round(limit, 3), 'type': m.kind, 'line': m.line}))
            continue
        top_hit, bottom_hit, deep = _windows(model, s, m)
        if m.z - m.h < HEIGHT_TOL_MM:
            # On the bed: a body island keeps the first layer's height (its speeds go by it); a contact
            # layer there may be banded, and its island reaches the model. Judged against the H/2-off slice.
            if natural is not None and m.kind == 'Support':
                bed_paths += 1
                layer = natural.interval(m.x0, m.y0, m.z)
                if natural.banded(m) and layer and not _island_reaches_model(coarse, s, m.x0, m.y0, *layer, span=2):
                    report.add(issue('first_layer', 'support body on the bed printed in a band under range "contact"',
                                     m.z, (m.x0, m.y0), {'height': m.h, 'off_height': natural.height(m.x0, m.y0, m.z),
                                                         'line': m.line}))
            continue
        if top_hit or bottom_hit:
            window_paths += 1
            if m.h > limit:
                hit = top_hit or bottom_hit
                report.add(issue('contact_top' if top_hit else 'contact_bottom',
                                 f'support {"under" if top_hit else "on"} the model within the contact window at a whole layer',
                                 m.z, hit[1:], {'height': m.h, 'band_limit': round(limit, 3), 'model_mm': hit[0],
                                                'window_mm': round(s.window_top if top_hit else s.window_bottom, 3),
                                                'type': m.kind, 'line': m.line}))
        elif deep and m.kind == 'Support' and m.z > s.first + 2 * s.layer and _deep_island(coarse, s, m):
            deep_mm += m.length
            if m.h >= WHOLE_SHARE * model.whole_layer(m.z, s.layer):
                deep_whole_mm += m.length
    if s.scope == 'contact' and deep_mm >= DEEP_MIN_MM and deep_whole_mm == 0:
        report.add(issue('deep_body', 'none of the deep support body prints at a whole layer under range "contact"',
                         measured={'deep_body_mm': round(deep_mm, 1)}))
    report.points['heights'] = {'support_paths': len(support), 'window_paths': window_paths, 'bed_body_paths': bed_paths,
                                'deep_body_mm': round(deep_mm, 1), 'deep_whole_mm': round(deep_whole_mm, 1)}


def check_islands(g, s, report, natural, geo=None):
    """island: under range "contact" every banded support island reaches a model contact.

    Banded = thinner than the H/2-off slice's support layer at that XY and Z. Islands are rebuilt per
    print Z from the banded paths on a coarse raster whose cells are a fill's reach wide, joined where
    they touch. Each must have the model over or under some cell of it or a neighbour within the
    contact window plus a layer (a band's top lies up to a layer under its island's top).
    """
    if s.scope != 'contact' or natural is None:
        return
    size = max(CELL_MM, s.island_reach)
    model = ModelIndex(geo or g, None, size)
    # Interface paths per coarse cell: a body island under or beside an interface band in its own Z is banded with it
    # (the modulation would otherwise trim it where the band's lines do not run).
    interfaces = {}
    for m in g.support():
        if m.kind == 'Support interface':
            for x, y in m.samples():
                interfaces.setdefault(cell(x, y, size), set()).add((round(m.z - m.h, 4), round(m.z, 4)))

    def beside_interface(x, y, bottom, top):
        c = cell(x, y, size)
        return any(min(top, it) - max(bottom, ib) > HEIGHT_TOL_MM
                   for dx in (-1, 0, 1) for dy in (-1, 0, 1) for ib, it in interfaces.get((c[0] + dx, c[1] + dy), ()))
    by_z = {}
    for m in g.support():
        if s.raft and m.z < model.lowest - HEIGHT_TOL_MM:
            continue
        # Body islands: interfaces are banded whatever their reach (their area runs past the model's).
        if m.z - m.h >= HEIGHT_TOL_MM and m.kind != 'Support interface' and natural.banded(m):
            by_z.setdefault(round(m.z, 4), []).append(m)
    islands = 0
    for z, moves in sorted(by_z.items()):
        cells = {}
        for m in moves:
            for x, y in m.samples():
                cells.setdefault(cell(x, y, size), []).append(m)
        seen = set()
        for start in cells:
            if start in seen:
                continue
            island, stack = [], [start]
            seen.add(start)
            while stack:
                c = stack.pop()
                island.append(c)
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        n = (c[0] + dx, c[1] + dy)
                        if n in cells and n not in seen:
                            seen.add(n)
                            stack.append(n)
            islands += 1
            # The window counts from the island's own support layer (the off slice's layer holding the
            # band), else from the band with a layer's slack.
            touched = False
            for c in island:
                for m in cells[c]:
                    layer = natural.interval(m.x0, m.y0, m.z)
                    bottom, top = layer if layer else (m.z - m.h - s.layer, m.z + s.layer)
                    if _island_reaches_model(model, s, m.x0, m.y0, bottom, top) or beside_interface(m.x0, m.y0, bottom, top):
                        touched = True
                        break
                if touched:
                    break
            if not touched:
                m = cells[island[0]][0]
                report.add(issue('island', 'banded support island with no model contact within the window', z,
                                 (m.x0, m.y0), {'island_cells': len(island), 'cell_mm': round(size, 2),
                                                'heights': sorted({round(p.h, 3) for c in island for p in cells[c]}),
                                                'off_height': natural.height(m.x0, m.y0, z), 'line': m.line}))
    report.points['islands'] = {'banded_islands': islands}


def check_columns(g, s, report):
    """overlap: two support paths with different tops overlap in Z at one XY (fine raster, centrelines).
    gap: a band-sized Z hole in the support of a 3 x 3 coarse neighbourhood (the fill pattern of one
    band and the next differ, so one coarse cell alone has holes by design)."""
    support = g.support()
    if not support:
        return
    fine, coarse = {}, {}
    model = ModelIndex(g, None)
    for m in support:
        if s.raft and m.z < model.lowest - HEIGHT_TOL_MM:
            continue   # the raft and the gap over it
        bottom = m.z - m.h
        for x, y in m.samples():
            # A sliver has no material to overlap with; it still fills its column (resin tips are this small).
            if m.e * s.filament_area >= MIN_VOLUME_MM3:
                fine.setdefault(cell(x, y), []).append((bottom, m.z, m))
            coarse.setdefault(cell(x, y, GAP_CELL_MM), set()).add((round(bottom, 4), round(m.z, 4)))
    for c, spans in fine.items():
        spans.sort(key=lambda s_: (s_[0], s_[1]))
        for i in range(1, len(spans)):
            b, t, m = spans[i]
            pb, pt, pm = spans[i - 1]
            if abs(t - pt) > HEIGHT_TOL_MM and min(t, pt) - max(b, pb) > OVERLAP_MM:
                report.add(issue('overlap', 'two support paths overlap in Z at one XY (double extrusion)', t,
                                 (c[0] * CELL_MM, c[1] * CELL_MM),
                                 {'a': [round(pb, 3), round(pt, 3), pm.line], 'b': [round(b, 3), round(t, 3), m.line]}))
                break
    for c in coarse:
        spans = set()
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                spans |= coarse.get((c[0] + dx, c[1] + dy), set())
        own = coarse[c]
        lo, hi = min(b for b, _ in own), max(t for _, t in own)
        spans = sorted(sp for sp in spans if sp[1] > lo - HEIGHT_TOL_MM and sp[0] < hi + HEIGHT_TOL_MM)
        reach = spans[0][1]
        for b, t in spans[1:]:
            hole = b - reach
            if hole > GAP_MIN_MM and lo < b < hi:
                limit = 0.5 * model.whole_layer(b, s.layer) + HEIGHT_TOL_MM
                if hole <= limit:
                    report.add(issue('gap', 'band-sized Z hole in the support of a column', b,
                                     (c[0] * GAP_CELL_MM, c[1] * GAP_CELL_MM),
                                     {'hole_mm': round(hole, 3), 'from': round(reach, 3), 'to': round(b, 3)}))
                    break
            reach = max(reach, t)
    report.points['columns'] = {'fine_cells': len(fine), 'coarse_cells': len(coarse)}


def check_flow(g, s, report):
    checked = 0
    for m in g.support():
        length = m.length
        if length < FLOW_MIN_LEN_MM or m.h <= 0 or m.w <= 0:
            continue
        bead = m.h * (m.w - m.h * (1 - math.pi / 4))   # rounded-rectangle bead, mm2
        if bead <= 0:
            continue
        ratio = m.e * s.filament_area / length / bead
        checked += 1
        if not (FLOW_LOW <= ratio <= FLOW_HIGH):
            report.add(issue('flow', 'support extrusion per mm does not match its width and height', m.z, (m.x0, m.y0),
                             {'ratio': round(ratio, 3), 'height': m.h, 'width': m.w, 'type': m.kind, 'line': m.line}))
    report.points['flow'] = {'paths': checked}


def check_z_safety(g, s, report):
    """The nozzle (a path's Z, a travel's Z) never runs below material printed earlier at the same XY
    (centreline cells)."""
    tops = {}
    travels = extrusions = 0
    for m in g.moves:
        hit = None
        for x, y in m.samples():
            t = tops.get(cell(x, y))
            if t is not None and t - m.z > Z_SAFETY_MM:
                hit = (x, y, t)
                break
        if hit:
            what = 'travel through printed material' if m.kind is None else 'extrusion below material printed earlier'
            report.add(issue('z_safety', what, m.z, hit[:2],
                             {'nozzle_z': m.z, 'material_top': hit[2], 'type': m.kind, 'line': m.line}))
        if m.kind is None:
            travels += 1
            continue
        extrusions += 1
        for x, y in m.samples():
            c = cell(x, y)
            if tops.get(c, -1.0) < m.z:
                tops[c] = m.z
    report.points['z_safety'] = {'travels': travels, 'extrusions': extrusions}


def check_tower_height(g, s, report):
    """Every tower layer after the first: bead height = Z step from the tower's previous layer."""
    layers = {}
    for m in g.moves:
        if m.kind == 'Prime tower':
            layers.setdefault(round(m.z, 3), set()).add(round(m.h, 3))
    # The first layer rises from the bed when it starts within a nozzle of it.
    previous = 0.0 if layers and min(layers) <= g.number('nozzle_diameter', 0.4) + TOWER_TOL_MM else None
    for z in sorted(layers):
        if previous is not None:
            rise = z - previous
            wrong = sorted(h for h in layers[z] if abs(h - rise) > TOWER_TOL_MM)
            if wrong:
                report.add(issue('tower_height', 'tower layer height differs from its rise', z, None,
                                 {'heights': wrong, 'rise_mm': round(rise, 3)}))
        previous = z
    report.points['tower'] = {'layers': len(layers)}


def check_one(path, scope, off_path=None):
    """All single-G-code checks. scope: "contact" or "all"; off_path: the same case with H/2 off."""
    g = Gcode(path)
    s = Settings(g, scope)
    off = Gcode(off_path) if off_path else None
    natural = NaturalIndex(off, max(CELL_MM, s.island_reach)) if off else None
    report = Report()
    check_integrity(g, report)
    check_heights(g, s, report, natural, off)
    check_islands(g, s, report, natural, off)
    check_columns(g, s, report)
    check_flow(g, s, report)
    check_z_safety(g, s, report)
    check_tower_height(g, s, report)
    return report.as_dict()


def footprint(g, size):
    """Per Z bin (a quarter layer), the coarse cells of the support paths whose [z - height, z]
    overlaps the bin's middle half by more than COVER_Z_MM (band ends rounded to 0.001 mm then
    never decide a bin)."""
    out = {}
    step = 0.25 * g.number('layer_height', 0.2)
    for m in g.support():
        b, t = m.z - m.h, m.z
        cells = None
        for k in range(int(math.floor(b / step)) - 1, int(math.ceil(t / step)) + 1):
            lo, hi = (k + 0.25) * step, (k + 0.75) * step
            if min(t, hi) - max(b, lo) > COVER_Z_MM:
                if cells is None:
                    cells = {cell(x, y, size) for x, y in m.samples()}
                out.setdefault(k, set()).update(cells)
    return out, step


def compare_pair(contact_path, all_path, off_path=None):
    """coverage (against "all" and H/2 off), model and time of a "contact" G-code against "all"."""
    gc, ga = Gcode(contact_path), Gcode(all_path)
    report = Report()
    # Footprints differ in where their sparse lines run (a band, a merged layer and a whole layer fill
    # at their own angles), not in the area they cover: cells as wide as the pattern's line pitch.
    size = max(CELL_MM, Settings(gc, 'contact').pattern_pitch)
    fc, step = footprint(gc, size)
    worst = 0
    first_top = Settings(gc, 'contact').first
    fa, _ = footprint(ga, size)
    fo = footprint(Gcode(off_path), size)[0] if off_path else None
    # coverage: against "all" (what the range changes); coverage_off: against H/2 off (what H/2 support
    # changes at all). Each issue says which of the three agree, so a fault of "all" is told apart.
    for check, other_name, fother in (('coverage', 'all', fa), ('coverage_off', 'off', fo)):
        if fother is None:
            continue
        for k in sorted(set(fc) | set(fother)):
            # "all" bands the first layer, "contact" keeps its bed islands whole: compared with off.
            if other_name == 'all' and (k + 0.25) * step < first_top:
                continue
            a, b = fc.get(k, set()), fother.get(k, set())
            diff = len(a ^ b) * size * size
            worst = max(worst, diff)
            whole = max(len(a), len(b)) * size * size
            if diff > COVER_MIN_MM2 if other_name == 'all' else (diff > COVER_OFF_MIN_MM2 and diff > COVER_OFF_SHARE * whole):
                measured = {'contact_mm2': round(len(a) * size * size, 1),
                            f'{other_name}_mm2': round(len(b) * size * size, 1), 'differs_mm2': round(diff, 1)}
                if fo is not None and other_name == 'all':
                    measured['contact_matches_off_mm2'] = round(len(a ^ fo.get(k, set())) * size * size, 1)
                    measured['all_matches_off_mm2'] = round(len(b ^ fo.get(k, set())) * size * size, 1)
                some = sorted(a ^ b)[0]
                report.add(issue(check, f'support footprint differs from {other_name}', (k + 0.5) * step,
                                 (some[0] * size, some[1] * size), measured))

    # The object's paths: same type, Z, height, ends and extrusion in both (any order).
    def model_paths(g):
        return sorted((m.kind, round(m.z, 3), round(m.h, 3), round(m.x0, 2), round(m.y0, 2), round(m.x1, 2),
                       round(m.y1, 2), round(m.e, 4)) for m in g.model())
    mc, ma = model_paths(gc), model_paths(ga)
    if mc != ma:
        differ = sorted(set(mc) ^ set(ma))
        first = differ[0] if differ else None
        report.add(issue('model', 'object paths differ between ranges "contact" and "all"', first[1] if first else None,
                         first[3:5] if first else None,
                         {'contact_only': len(set(mc) - set(ma)), 'all_only': len(set(ma) - set(mc)),
                          'paths': [len(mc), len(ma)], 'first': first}))
    if gc.time_s is not None and ga.time_s is not None and gc.time_s > ga.time_s * (1 + TIME_SHARE):
        report.add(issue('time', 'range "contact" takes longer than "all"',
                         measured={'contact_s': gc.time_s, 'all_s': ga.time_s}))
    report.points['pair'] = {'worst_footprint_mm2': round(worst, 1), 'contact_s': gc.time_s, 'all_s': ga.time_s}
    return report.as_dict()
