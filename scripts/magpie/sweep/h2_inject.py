#!/usr/bin/env python3
"""Counterfactuals for the H/2 support checks (h2_checks.py): each check must flag its defect.

Takes a random_sweep output folder sliced with a correct build, picks a "contact" case with its
"__all" and "__off" twins (and an "all" case), injects one defect per check into a copy of the
G-code, runs the checks and prints a table: check, injected defect, caught, issue count before and
after. The unmodified G-code must pass the same check (the "before" column), so a catch is the
defect's and not the case's.

    python h2_inject.py <sweep out dir> [--case case_005] [--all-case case_003]
"""
import argparse
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import h2_checks as H  # noqa: E402


def gcode_of(out, name):
    d = os.path.join(out, 'run1', name, 'out')
    g = sorted(f for f in os.listdir(d) if f.endswith('.gcode')) if os.path.isdir(d) else []
    return os.path.join(d, g[0]) if g else None


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read().split('\n')


def write(lines, tmp, name):
    path = os.path.join(tmp, name + '.gcode')
    with open(path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))
    return path


def with_height(lines, move, height):
    """The move printed at another ;HEIGHT (the tag before it, the old one restored after it)."""
    out = list(lines)
    i = move.line - 1
    out[i:i + 1] = [f';HEIGHT:{height:.3f}', lines[i], f';HEIGHT:{move.h:.3f}']
    return out


def scale_e(line, factor):
    words = []
    for w in line.split(' '):
        if w.startswith('E'):
            w = f'E{float(w[1:]) * factor:.5f}'
        words.append(w)
    return ' '.join(words)


def no_e(line):
    return ' '.join(w for w in line.split(';', 1)[0].split(' ') if not w.startswith('E'))


def run(path, scope, off=None):
    return H.check_one(path, scope, off)


def count(result, check):
    return result['counts'].get(check, 0)


def pick(moves, predicate):
    for m in moves:
        if predicate(m):
            return m
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('out')
    p.add_argument('--case')
    p.add_argument('--all-case')
    a = p.parse_args()
    corpus = {c['name']: c for c in json.load(open(os.path.join(a.out, 'corpus.json')))['cases']}

    def scope_of(c):
        mods = json.loads(c['process'].get('mod_settings', '{}'))
        on = c['process'].get('enable_support') == '1' and (mods.get('half_layer.support') or mods.get('half_layer.outer_walls'))
        return mods.get('half_layer.support_scope', 'contact') if on else None

    # A "contact" case with twins, a deep body, a contact zone and no raft (the richest to mutate).
    contact = a.case
    if contact is None:
        best = None
        for name, c in corpus.items():
            if c.get('pair_of') or scope_of(c) != 'contact' or c['process'].get('raft_layers'):
                continue
            if not (gcode_of(a.out, name) and gcode_of(a.out, name + '__all') and gcode_of(a.out, name + '__off')):
                continue
            r = run(gcode_of(a.out, name), 'contact', gcode_of(a.out, name + '__off'))
            pts = r['points'].get('heights', {})
            if r['counts']:
                continue
            score = min(pts.get('window_paths', 0), 50) + min(pts.get('deep_whole_mm', 0), 50)
            if best is None or score > best[0]:
                best = (score, name)
        contact = best[1]
    all_case = a.all_case or next(n for n, c in corpus.items()
                                  if not c.get('pair_of') and scope_of(c) == 'all' and gcode_of(a.out, n)
                                  and any(m.z - m.h > H.HEIGHT_TOL_MM for m in H.Gcode(gcode_of(a.out, n)).support()))
    gc, ga, go = gcode_of(a.out, contact), gcode_of(a.out, contact + '__all'), gcode_of(a.out, contact + '__off')
    gall = gcode_of(a.out, all_case)
    print(f'contact case {contact} ({corpus[contact]["model"]}), all case {all_case}')

    g = H.Gcode(gc)
    s = H.Settings(g, 'contact')
    support = g.support()
    model_cells = H.ModelIndex(g, {H.cell(x, y) for m in support for x, y in m.samples()})
    coarse = H.ModelIndex(g, None, max(H.CELL_MM, s.island_reach))
    natural = H.NaturalIndex(H.Gcode(go), max(H.CELL_MM, s.island_reach))
    lines = read(gc)
    base = run(gc, 'contact', go)
    base_pair = H.compare_pair(gc, ga, go)
    tmp = tempfile.mkdtemp(prefix='h2inject_')
    rows = []

    def record(check, defect, result, before):
        rows.append((check, defect, count(result, check) > 0, count(before, check), count(result, check)))

    # integrity: the G-code cut off at 70 %.
    path = write(lines[:int(len(lines) * 0.7)], tmp, 'truncated')
    record('integrity', 'G-code cut at 70 % of its lines', run(path, 'contact', go), base)

    def contact_cases():
        # This case first, then the other "contact" cases with twins and no raft.
        yield contact
        for name, c in corpus.items():
            if name != contact and not c.get('pair_of') and scope_of(c) == 'contact' and not c['process'].get('raft_layers') \
                    and gcode_of(a.out, name) and gcode_of(a.out, name + '__off'):
                yield name

    def find(predicate):
        """(case, its lines, its off twin, its result unchanged, the move, context) of the first case
        that passes unchanged and has a path matching predicate(move, context)."""
        for name in contact_cases():
            gg = H.Gcode(gcode_of(a.out, name))
            ss = H.Settings(gg, 'contact')
            sup = gg.support()
            reach = max(H.CELL_MM, ss.island_reach)
            ctx = {'s': ss, 'model': H.ModelIndex(gg, {H.cell(x, y) for k in sup for x, y in k.samples()}),
                   'coarse': H.ModelIndex(gg, None, reach),
                   'natural': H.NaturalIndex(H.Gcode(gcode_of(a.out, name + '__off')), reach)}
            m = pick(sup, lambda k: predicate(k, ctx))
            if m is None:
                continue
            off_ = gcode_of(a.out, name + '__off')
            before = run(gcode_of(a.out, name), 'contact', off_)
            if not before['counts']:
                return name, read(gcode_of(a.out, name)), off_, before, m, ctx
        return None

    # contact_top / contact_bottom: a banded path within the window printed at a whole layer.
    for side in ('top', 'bottom'):
        def in_window(m, ctx, side=side):
            ss = ctx['s']
            if m.z - m.h < H.HEIGHT_TOL_MM or m.h > 0.5 * ss.layer + H.HEIGHT_TOL_MM:
                return False
            t_, b_, _ = H._windows(ctx['model'], ss, m)
            return t_ if side == 'top' else (b_ and not t_)
        found = find(in_window)
        if found is None:
            rows.append((f'contact_{side}', 'no support path within the window on that side in any case', None, None, None))
            continue
        name, ls, off_, before, m, ctx = found
        path = write(with_height(ls, m, ctx['s'].layer), tmp, f'contact_{side}')
        where = 'under' if side == 'top' else 'on'
        record(f'contact_{side}', f'{name}: band at Z {m.z:.3f} {where} the model printed {ctx["s"].layer} mm high',
               run(path, 'contact', off_), before)

    # all_banded: one support path of an "all" case printed at a whole layer.
    ga_g = H.Gcode(gall)
    m = pick(ga_g.support(), lambda m: m.z - m.h > H.HEIGHT_TOL_MM)
    whole = H.ModelIndex(ga_g).whole_layer(m.z, H.Settings(ga_g, 'all').layer)
    path = write(with_height(read(gall), m, whole), tmp, 'all_banded')
    record('all_banded', f'one band at Z {m.z:.3f} of an "all" case printed {whole:.3f} mm high (its model layer)',
           run(path, 'all'), run(gall, 'all'))

    # deep_body: the range ignored (every support banded): the "__all" G-code judged as "contact".
    record('deep_body', 'range ignored: the case\'s "all" G-code under "contact"', run(ga, 'contact', go), base)

    # island: a whole-height path of the deep body printed in a band.
    found = find(lambda m, ctx: m.kind == 'Support' and m.z > ctx['s'].first + 3 * ctx['s'].layer and m.length > 0.5
                 and ctx['natural'].banded(m) is False and H._deep_island(ctx['coarse'], ctx['s'], m))
    if found is None:
        rows.append(('island', 'no deep whole-height body path in any case', None, None, None))
    else:
        name, ls, off_, before, m, ctx = found
        path = write(with_height(ls, m, 0.5 * m.h), tmp, 'island')
        record('island', f'{name}: deep body path at Z {m.z:.3f} printed as a {0.5 * m.h:.3f} mm band',
               run(path, 'contact', off_), before)

    # first_layer: a bed body path printed as the lower band of a banded first layer (on the bed,
    # half the height).
    found = find(lambda m, ctx: m.kind == 'Support' and m.z - m.h < H.HEIGHT_TOL_MM and m.length > 2
                 and ctx['natural'].banded(m) is False
                 and not H._island_reaches_model(ctx['coarse'], ctx['s'], m.x0, m.y0, 0.0, m.z, span=2))
    if found is None:
        rows.append(('first_layer', 'no bed body path away from the model in any case', None, None, None))
    else:
        name, ls, off_, before, m, ctx = found
        i = m.line - 1
        out = ls[:i] + [f'G1 Z{0.5 * m.z:.3f}', f';HEIGHT:{0.5 * m.h:.3f}', ls[i], f'G1 Z{m.z:.3f}',
                        f';HEIGHT:{m.h:.3f}'] + ls[i + 1:]
        path = write(out, tmp, 'first_layer')
        record('first_layer', f'{name}: bed body path printed as a {0.5 * m.h:.3f} mm band on the bed',
               run(path, 'contact', off_), before)

    # overlap: a whole-height body path printed again as a band inside its own layer.
    m = pick(support, lambda m: m.kind == 'Support' and m.length > 0.5 and m.z - m.h > H.HEIGHT_TOL_MM
             and natural.banded(m) is False)
    i = m.line - 1
    extra = [f'G1 X{m.x0:.3f} Y{m.y0:.3f}', f'G1 Z{m.z - 0.5 * m.h:.3f}', f';HEIGHT:{0.5 * m.h:.3f}', lines[i],
             f'G1 Z{m.z:.3f}', f';HEIGHT:{m.h:.3f}']
    path = write(lines[:i + 1] + extra + lines[i + 1:], tmp, 'overlap')
    record('overlap', f'body path at Z {m.z:.3f} extruded again as a band inside it', run(path, 'contact', go), base)

    # gap: every support path of one band Z (in a banded stretch) left out.
    bands = sorted({round(m.z, 4) for m in support if m.z - m.h > H.HEIGHT_TOL_MM and natural.banded(m)})
    zs = sorted({round(m.z, 4) for m in support})
    z = next((b for b in bands if zs.index(b) not in (0, len(zs) - 1)), None)
    if z is None:
        rows.append(('gap', 'no banded Z with support above and below', None, None, None))
    else:
        drop = {m.line - 1 for m in support if abs(m.z - z) < 1e-4}
        path = write([no_e(l) if i in drop else l for i, l in enumerate(lines)], tmp, 'gap')
        record('gap', f'all support of band Z {z:.3f} left out', run(path, 'contact', go), base)

    # flow: one support path extruding 1.5 x.
    m = pick(support, lambda m: m.length > 3 and m.z - m.h > H.HEIGHT_TOL_MM)
    out = list(lines)
    out[m.line - 1] = scale_e(lines[m.line - 1], 1.5)
    path = write(out, tmp, 'flow')
    record('flow', f'support path at Z {m.z:.3f} extruding 1.5 x', run(path, 'contact', go), base)

    # z_safety: a travel 0.1 mm under the top of the support path just printed, across it.
    m = pick(support, lambda m: m.length > 3 and m.z > s.first + 2 * s.layer)
    i = m.line - 1
    extra = [f'G1 Z{m.z - 0.1:.3f}', f'G1 X{m.x0:.3f} Y{m.y0:.3f}', f'G1 Z{m.z:.3f}', f'G1 X{m.x1:.3f} Y{m.y1:.3f}']
    path = write(lines[:i + 1] + extra + lines[i + 1:], tmp, 'z_safety')
    record('z_safety', f'travel 0.1 mm under printed support at Z {m.z:.3f}', run(path, 'contact', go), base)

    # coverage: one support island (paths within 4 mm of a body path) left out over every Z.
    m = pick(support, lambda m: m.kind == 'Support' and m.length > 2 and m.z > s.first + 2 * s.layer)
    cx, cy = m.x0, m.y0
    drop = {k.line - 1 for k in support if any(((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 < 4.0 for x, y in k.samples())}
    path = write([no_e(l) if i in drop else l for i, l in enumerate(lines)], tmp, 'coverage')
    record('coverage', f'support within 4 mm of ({cx:.1f}, {cy:.1f}) left out, {len(drop)} paths',
           H.compare_pair(path, ga, go), base_pair)

    # model: one object path extruding 1.1 x.
    m = pick(g.model(), lambda m: m.length > 3)
    out = list(lines)
    out[m.line - 1] = scale_e(lines[m.line - 1], 1.1)
    path = write(out, tmp, 'model')
    record('model', f'object path ({m.kind}) at Z {m.z:.3f} extruding 1.1 x', H.compare_pair(path, ga, go), base_pair)

    # time: "contact" estimated 5 % longer than "all".
    all_s = H.Gcode(ga).time_s
    out = [f'; estimated printing time (normal mode) = {int(all_s * 1.05) // 60}m {int(all_s * 1.05) % 60}s'
           if l.startswith('; estimated printing time (normal mode)') else l for l in lines]
    path = write(out, tmp, 'time')
    record('time', f'estimated time {int(all_s * 1.05)} s against {all_s} s for "all"', H.compare_pair(path, ga, go), base_pair)

    shutil.rmtree(tmp, ignore_errors=True)
    print(f'{"check":<15} {"caught":<7} {"before":>6} {"after":>6}  injected defect')
    for check, defect, caught, before, after in rows:
        mark = 'n/a' if caught is None else 'YES' if caught else 'NO'
        print(f'{check:<15} {mark:<7} {"" if before is None else before:>6} {"" if after is None else after:>6}  {defect}')
    return 0 if all(r[2] in (True, None) and (r[3] in (0, None)) for r in rows) else 1


if __name__ == '__main__':
    sys.exit(main())
