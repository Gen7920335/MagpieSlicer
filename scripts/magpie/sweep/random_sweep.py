#!/usr/bin/env python3
"""Random setting and mod combination sweep (P6; Magpie line 1 checked 204 combinations).

Builds N random cases from a seed: model, support generator and type, mods (half-height walls
and support with its range, low temperature interface and tower, support extras, multi-nozzle
walls on a mixed nozzle U1), wall generator and count, layer height, infill, interface layers
under H/2 support. Slices them with the golden harness, re-slices every fourth case to check
determinism, and checks every G-code:

- the CLI either slices or rejects the settings with a message; a crash (with its Windows
  exception code and crash log), a slicing error, a timeout, a slice over the memory cap
  (Windows job object, 16 GB by default) or a mod that did not load fails, each reported in its
  own category;
- XY stays on the bed (with 1 mm slack), Z stays between 0 and the bed height;
- nozzle and bed temperatures stay within 0-350 degC;
- no NaN or infinite numbers;
- extrusion amounts (relative E) per move stay below 50 mm of filament;
- H/2 support (half_layer.support or outer_walls with support on): the checks of h2_checks.py,
  read from the G-code alone. A "contact" case is also sliced with "all" and with H/2 off
  (<case>__all, <case>__off) for the comparisons. h2_inject.py shows each check catching its
  defect on a copy of a passing G-code.

--h2-support forces support with H/2 support on in every case; --regressions adds pinned cases
that reproduced past defects (they need their models in --external-models and are skipped
otherwise). H/2 cases now and then use a generated ledge model, so support stands on the object.

    python random_sweep.py --cli <magpie-slicer.exe> --datadir <golden datadir> \
        --external-models <dir with tee.stl> --out <dir> --count 204 --seed 1
"""
import argparse
import glob
import json
import os
import random
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'golden'))
sys.path.insert(0, HERE)
import golden  # noqa: E402
import h2_checks  # noqa: E402

MODELS = ['tests/data/20mm_cube.obj', 'tests/data/overhang.obj', 'tests/data/frog_legs.obj', 'external:tee.stl']
BED_MM = 270.0         # U1 printable area, mm
BED_SLACK_MM = 1.0     # moves may touch the edge by this much, mm
MAX_TEMP_C = 350.0     # highest believable nozzle or bed temperature, degC
MAX_E_PER_MOVE_MM = 50.0  # filament per single move, mm

# Pinned cases that reproduced past defects (--regressions). hilda03 under H/2 support of the
# contact zones crashed in the support modulation (ACCESS_VIOLATION) before 66c0a3b689.
REGRESSION_CASES = [
    {'name': f'reg_hilda03_{support}_contact', 'model': 'external:hilda03.stl', 'optional': True, 'printer': 'u1',
     'process': dict({'enable_support': '1', 'support_interface_top_layers': '2', 'support_interface_bottom_layers': '2',
                      'mod_settings': json.dumps({'half_layer.support': True, 'half_layer.support_scope': 'contact'},
                                                 sort_keys=True)}, **style)}
    for support, style in (('normal', {'support_type': 'normal(auto)'}),
                           ('organic', {'support_type': 'tree(auto)', 'support_style': 'tree_organic'}))
]

# Windows exception codes a crashed slicer exits with (NTSTATUS, unsigned).
WINDOWS_EXCEPTIONS = {
    0xC0000005: 'ACCESS_VIOLATION', 0xC000001D: 'ILLEGAL_INSTRUCTION', 0xC0000094: 'INTEGER_DIVIDE_BY_ZERO',
    0xC00000FD: 'STACK_OVERFLOW', 0xC0000374: 'HEAP_CORRUPTION', 0xC0000409: 'STACK_BUFFER_OVERRUN',
    0xC0000417: 'INVALID_CRUNTIME_PARAMETER', 0x80000003: 'BREAKPOINT', 0xE06D7363: 'CPP_EXCEPTION',
    0x40000015: 'ABORT',
}


def write_ledge_model(out):
    """A ledge whose overhang's support stands on the object: slab 20 x 10 x 3 mm on the bed, pillar
    5 x 10 mm on it up to Z 9 mm, ceiling 20 x 10 x 1.2 mm from Z 9 mm. Written into the output folder
    (binary STL); returns its absolute path."""
    def box(x0, y0, z0, x1, y1, z1):
        v = [(x, y, z) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)]
        quads = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
        return [tri for a, b, c, d in quads for tri in ((v[a], v[b], v[c]), (v[a], v[c], v[d]))]
    tris = box(0, 0, 0, 20, 10, 3) + box(0, 0, 3, 5, 10, 9) + box(0, 0, 9, 20, 10, 10.2)
    path = os.path.join(out, 'h2_ledge.stl')
    with open(path, 'wb') as f:
        f.write(b'\0' * 80 + struct.pack('<I', len(tris)))
        for tri in tris:
            f.write(struct.pack('<3f', 0, 0, 0))
            for p in tri:
                f.write(struct.pack('<3f', *p))
            f.write(b'\0\0')
    return path


def random_case(rng, index, seed=1, force_h2_support=False, ledge_model=None):
    # New draws come from their own generator, so the cases of a seed stay the same as before.
    rng_h2 = random.Random(f'h2:{seed}:{index}')
    multi = rng.random() < 0.2
    support = rng.choice(['off', 'normal(auto)', 'tree(auto)'])
    if force_h2_support and support == 'off':
        support = rng_h2.choice(['normal(auto)', 'tree(auto)'])
    process = {
        'layer_height': str(rng.choice([0.12, 0.16, 0.2, 0.24, 0.28])),
        'wall_generator': rng.choice(['classic', 'arachne']),
        'wall_loops': str(rng.randint(1, 4)),
        'sparse_infill_density': f'{rng.choice([0, 10, 15, 25, 40])}%',
        'sparse_infill_pattern': rng.choice(['grid', 'gyroid', 'crosshatch', 'rectilinear', 'honeycomb']),
        'enable_support': '0' if support == 'off' else '1',
    }
    if support != 'off':
        process['support_type'] = support
    mods = {}
    if rng.random() < 0.3:
        mods['half_layer.outer_walls'] = True
    if rng.random() < 0.3 or force_h2_support:
        mods['half_layer.support'] = True
    if support != 'off':
        generator = rng.choice(['orca', 'orca', 'cura', 'mixed', 'resin'])
        if generator != 'orca':
            mods[f'{generator}_support.enable'] = True
        if generator == 'mixed':
            mods['mixed_support.normal_generator'] = rng.choice(['orca', 'cura'])
        if generator == 'resin':
            mods['resin_support.tree_type'] = rng.choice(['default', 'branching'])
        if rng.random() < 0.3:
            mods['low_temp_interface.enable'] = True
            mods['low_temp_interface.tower'] = rng.random() < 0.5
            process['support_interface_top_layers'] = str(rng.randint(1, 3))
        if rng.random() < 0.3:
            mods['support_extras.interface_pattern'] = rng.choice(['triangles', 'grid', 'concentric'])
        if rng.random() < 0.2:
            mods['support_extras.normal_wall_loops'] = rng.randint(0, 2)
            mods['support_extras.tree_wall_loops'] = rng.randint(1, 3)
        if mods.get('half_layer.support') or mods.get('half_layer.outer_walls'):
            # H/2 support range: "contact" is the default, so it is drawn more often; interface layers
            # 0 and more both, since the contact window differs (low temperature interface keeps its own).
            mods['half_layer.support_scope'] = 'contact' if rng_h2.random() < 0.65 else 'all'
            if 'support_interface_top_layers' not in process:
                layers = rng_h2.choice([0, 0, 1, 2, 3])
                process['support_interface_top_layers'] = str(layers)
                process['support_interface_bottom_layers'] = str(layers)
            # Support standing on the object too, and a raft under it now and then.
            if rng_h2.random() < 0.5:
                process['support_on_build_plate_only'] = '0'
            if rng_h2.random() < 0.2:
                process['raft_layers'] = str(rng_h2.randint(1, 3))
    case = {'name': f'case_{index:03d}', 'model': rng.choice(MODELS), 'process': process}
    if ledge_model and support != 'off' and (mods.get('half_layer.support') or mods.get('half_layer.outer_walls')) \
            and rng_h2.random() < 0.25:
        # Support standing on the object (the bottom contact window): the ledge model.
        case['model'] = ledge_model
        process['support_on_build_plate_only'] = '0'
    if multi:
        case['printer'] = 'u1mn'
        case['machine'] = {'nozzle_diameter': ['0.4', '0.4', '0.6', '0.6'], 'max_layer_height': ['0.32', '0.32', '0.48', '0.48'],
                           'min_layer_height': ['0.08', '0.08', '0.12', '0.12'], 'nozzle_type': 'hardened_steel'}
        process.update({'bridge_line_width': '0.4', 'outer_wall_filament_id': '3', 'inner_wall_filament_id': '3',
                        'sparse_infill_filament_id': '3', 'internal_solid_filament_id': '3',
                        'top_surface_filament_id': '3', 'bottom_surface_filament_id': '3'})
        if rng.random() < 0.7:
            mods['multi_nozzle_walls.enable'] = True
            mods['multi_nozzle_walls.small_wall_count'] = rng.randint(1, 2)
            mods['multi_nozzle_walls.interlock'] = rng.random() < 0.3
        case['cli_args'] = ['--filament-colour', '#F2754E;#F2754E;#F2754E;#F2754E']
    else:
        case['printer'] = 'u1'
    if mods:
        process['mod_settings'] = json.dumps(mods, sort_keys=True)
    return case


def build_corpus(seed, count, force_h2_support=False, regressions=False, ledge_model=None):
    rng = random.Random(seed)
    cases = [random_case(rng, i, seed, force_h2_support, ledge_model) for i in range(count)]
    if regressions:
        cases += [dict(c) for c in REGRESSION_CASES]
    # Each "contact" case also slices with "all" (coverage, object paths, time) and with H/2 off
    # (the support's own layers, to tell a band from a thin layer).
    pairs = []
    for case in cases:
        on, scope = h2_settings(case)
        if on and scope == 'contact':
            mods = json.loads(case['process']['mod_settings'])
            for suffix, change in (('__all', {'half_layer.support_scope': 'all'}),
                                   ('__off', {'half_layer.support': False, 'half_layer.outer_walls': False})):
                twin = json.loads(json.dumps(case))
                twin['name'] = case['name'] + suffix
                twin['pair_of'] = case['name']
                twin['process']['mod_settings'] = json.dumps(dict(mods, **change), sort_keys=True)
                pairs.append(twin)
    cases += pairs
    return {
        'description': f'Random sweep, seed {seed}, {count} cases',
        'printers': {
            'u1': {'machine': 'presets/u1_04_machine.json', 'process': 'presets/u1_04_process.json',
                   'filaments': ['presets/u1_pla.json'], 'bed_type': 'High Temp Plate'},
            'u1mn': {'machine': 'presets/u1_04_machine.json', 'process': 'presets/u1_04_process.json',
                     'filaments': ['presets/u1_pla.json', 'presets/u1_pla_2.json', 'presets/u1_pla_3.json', 'presets/u1_pla_4.json'],
                     'bed_type': 'High Temp Plate'},
        },
        'cases': cases,
    }


def h2_settings(case):
    """(on, range) of H/2 support for a case, from its own input settings."""
    process = case.get('process', {})
    if process.get('enable_support') != '1':
        return False, None
    mods = json.loads(process.get('mod_settings', '{}'))
    if not (mods.get('half_layer.support') or mods.get('half_layer.outer_walls')):
        return False, None
    return True, mods.get('half_layer.support_scope', 'contact')


NUM = re.compile(r'([XYZEFS])(-?[0-9.]+(?:e-?[0-9]+)?|nan|inf|-inf)', re.IGNORECASE)


def check_gcode(path):
    problems = []
    relative_e = False
    with open(path, encoding='utf-8', errors='replace') as f:
        for n, raw in enumerate(f, 1):
            line = raw.split(';', 1)[0].strip()
            if not line:
                continue
            word = line.split()[0].upper()
            if word == 'M83':
                relative_e = True
            elif word == 'M82':
                relative_e = False
            if 'nan' in line.lower() or 'inf' in line.lower().replace('info', ''):
                problems.append(f'{n}: non-finite number: {line}')
                continue
            if word not in ('G0', 'G1', 'G2', 'G3', 'M104', 'M109', 'M140', 'M190'):
                continue
            values = {}
            for k, v in NUM.findall(line):
                try:
                    values[k.upper()] = float(v)
                except ValueError:
                    problems.append(f'{n}: malformed number: {line}')
            if word in ('G0', 'G1', 'G2', 'G3'):
                for axis in 'XY':
                    if axis in values and not (-BED_SLACK_MM <= values[axis] <= BED_MM + BED_SLACK_MM):
                        problems.append(f'{n}: {axis} off the bed: {line}')
                if 'Z' in values and not (0.0 <= values['Z'] <= BED_MM):
                    problems.append(f'{n}: Z out of range: {line}')
                if relative_e and 'E' in values and abs(values['E']) > MAX_E_PER_MOVE_MM:
                    problems.append(f'{n}: E move too long: {line}')
            elif word in ('M104', 'M109', 'M140', 'M190'):
                if 'S' in values and not (0.0 <= values['S'] <= MAX_TEMP_C):
                    problems.append(f'{n}: temperature out of range: {line}')
            if len(problems) >= 20:
                break
    return problems


def describe_exit(code):
    """The failure category of a CLI exit code and a readable form of it."""
    if code == 'timeout':
        return 'timeout', 'timeout'
    if code == 'mod-not-loaded':
        return 'mod_not_loaded', 'a mod did not load'
    if not isinstance(code, int):
        return 'crashed', str(code)
    unsigned = code & 0xFFFFFFFF
    if unsigned in WINDOWS_EXCEPTIONS or (unsigned >= 0xC0000000):
        return 'crashed', f'0x{unsigned:08X} {WINDOWS_EXCEPTIONS.get(unsigned, "exception")}'
    # Orca's CLI exits with a negative CLI_* code (Utils.hpp): -1..-63 reject the input or
    # settings; -100..-102 are slicing errors. Windows reports the code as unsigned 32-bit,
    # POSIX as its low byte; a POSIX signal shows as a negative code from subprocess.
    signed = code - (1 << 32) if code >= (1 << 31) else code - 256 if 128 <= code < 256 else code
    if -63 <= signed <= -1:
        return 'rejected', str(signed)
    if -102 <= signed <= -100:
        return 'slicing_error', str(signed)
    if signed < -102 or code < 0:
        return 'crashed', f'signal or code {signed}'
    return 'crashed', f'exit {code}'


def crash_logs(datadir, started, seconds):
    """Crash logs the slicer wrote into <datadir>/log while the case ran: (path, fault line)."""
    found = []
    for log in sorted(glob.glob(os.path.join(datadir, 'log', 'crash_*.log'))):
        mtime = os.path.getmtime(log)
        if started - 1 <= mtime <= started + seconds + 5:
            fault = ''
            with open(log, encoding='utf-8', errors='replace') as f:
                text = f.read()
            m = re.search(r'Show CallStack:\s*\n\s*\S+?:(.+)', text)
            if m:
                fault = m.group(1).strip()
            found.append({'log': log, 'fault': fault})
    return found


def first_gcode(out, run, name):
    gdir = os.path.join(out, run, name, 'out')
    if not os.path.isdir(gdir):
        return None
    g = sorted(f for f in os.listdir(gdir) if f.endswith('.gcode'))
    return os.path.join(gdir, g[0]) if g else None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cli', required=True)
    p.add_argument('--datadir', required=True)
    p.add_argument('--external-models')
    p.add_argument('--out', required=True)
    p.add_argument('--count', type=int, default=204)
    p.add_argument('--seed', type=int, default=1)
    p.add_argument('--timeout', type=int, default=900)
    p.add_argument('--memory-cap-gb', type=float, default=16.0, help='committed memory cap per slice (Windows), GB')
    p.add_argument('--h2-support', action='store_true', help='support with H/2 support on in every case')
    p.add_argument('--regressions', action='store_true', help='add the pinned cases of past defects')
    p.add_argument('--check-only', action='store_true', help='check the G-code of an earlier run in --out')
    a = p.parse_args()

    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    if a.check_only:
        # The cases the G-code was sliced from, not what this version of the script would draw.
        with open(os.path.join(out, 'corpus.json'), encoding='utf-8') as f:
            corpus = json.load(f)
    else:
        corpus = build_corpus(a.seed, a.count, a.h2_support, a.regressions, write_ledge_model(out))
        with open(os.path.join(out, 'corpus.json'), 'w', encoding='utf-8') as f:
            json.dump(corpus, f, indent=1)
    # The harness resolves presets relative to its own folder; keep the corpus beside it.
    corpus_path = os.path.join(golden.HERE, f'_sweep_{a.seed}.json')
    with open(corpus_path, 'w', encoding='utf-8') as f:
        json.dump(corpus, f, indent=1)
    datadir = os.path.abspath(a.datadir)
    cap_mb = int(a.memory_cap_gb * 1024) if a.memory_cap_gb > 0 else None
    repeat = {c['name'] for i, c in enumerate(corpus['cases'])
              if i % 4 == 0 and not c.get('optional') and not c.get('pair_of')}
    try:
        if a.check_only:
            raise FileExistsError
        golden.slice_corpus(a.cli, os.path.join(out, 'run1'), corpus_path, a.external_models, None, [], a.timeout,
                            datadir, cap_mb)
        if repeat:
            golden.slice_corpus(a.cli, os.path.join(out, 'run2'), corpus_path, a.external_models, repeat, [], a.timeout,
                                datadir, cap_mb)
        else:
            # Nothing to repeat (golden reads an empty selection as "all cases").
            os.makedirs(os.path.join(out, 'run2'), exist_ok=True)
            with open(os.path.join(out, 'run2', 'manifest.json'), 'w', encoding='utf-8') as f:
                json.dump({'cases': {}}, f)
    except FileExistsError:
        pass
    finally:
        os.remove(corpus_path)

    with open(os.path.join(out, 'run1', 'manifest.json'), encoding='utf-8') as f:
        run1 = json.load(f)['cases']
    with open(os.path.join(out, 'run2', 'manifest.json'), encoding='utf-8') as f:
        run2 = json.load(f)['cases']
    cases = {c['name']: c for c in corpus['cases']}
    report = {'sliced': [], 'rejected': [], 'crashed': [], 'slicing_error': [], 'timeout': [], 'memory_cap': [],
              'mod_not_loaded': [], 'skipped': [], 'gcode_problems': {}, 'nondeterministic': [],
              'h2': {'checked': 0, 'pairs': 0, 'counts': {}, 'cases': {}}}
    for name, r in run1.items():
        if r['status'] == 'ok':
            report['sliced'].append(name)
            problems = check_gcode(first_gcode(out, 'run1', name))
            if problems:
                report['gcode_problems'][name] = problems
            if name in run2 and run2[name].get('sha256') != r['sha256']:
                report['nondeterministic'].append(name)
        elif r['status'] == 'skipped':
            report['skipped'].append(name)
        else:
            code = r.get('exit')
            if code == 'memory-cap':
                category, what = 'memory_cap', f'over {cap_mb} MB (peak {r.get("peak_mb")} MB)'
            else:
                category, what = describe_exit(code)
            entry = {'case': name, 'exit': code, 'what': what, 'gcode': r.get('gcode')}
            if category == 'rejected':
                log = open(os.path.join(out, 'run1', name, 'cli.log'), encoding='utf-8', errors='replace').read()
                entry['log_tail'] = log.strip().splitlines()[-3:]
            if category == 'crashed' and 'started' in r:
                entry['crash_logs'] = crash_logs(datadir, r['started'], r.get('seconds', 0))
            report[category].append(entry)

    # H/2 support: every sliced case with H/2 support on. The "__all" and "__off" twins of a
    # "contact" case are checked through that case.
    h2 = report['h2']
    for name in report['sliced']:
        case = cases[name]
        on, scope = h2_settings(case)
        if not on or case.get('pair_of'):
            continue
        gpath = first_gcode(out, 'run1', name)
        off = first_gcode(out, 'run1', name + '__off') if scope == 'contact' else None
        result = h2_checks.check_one(gpath, scope, off)
        h2['checked'] += 1
        twin_all = first_gcode(out, 'run1', name + '__all') if scope == 'contact' else None
        if twin_all:
            h2['pairs'] += 1
            pair = h2_checks.compare_pair(gpath, twin_all, off)
            for k, v in pair['counts'].items():
                result['counts'][k] = result['counts'].get(k, 0) + v
            result['issues'].update(pair['issues'])
            result['points'].update(pair['points'])
        result['scope'] = scope
        h2['cases'][name] = result
        for k in result['counts']:
            h2['counts'][k] = h2['counts'].get(k, 0) + 1
    with open(os.path.join(out, 'report.json'), 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=1)
    print(f"sliced {len(report['sliced'])}, rejected {len(report['rejected'])}, crashed {len(report['crashed'])}, "
          f"slicing errors {len(report['slicing_error'])}, timeouts {len(report['timeout'])}, "
          f"memory cap {len(report['memory_cap'])}, mods not loaded {len(report['mod_not_loaded'])}, "
          f"skipped {len(report['skipped'])}, G-code problems {len(report['gcode_problems'])}, "
          f"nondeterministic {len(report['nondeterministic'])} (of {len(repeat)} repeated)")
    counts = json.dumps(h2['counts'], sort_keys=True) if h2['counts'] else 'none'
    print(f"H/2 support: {h2['checked']} cases checked, {h2['pairs']} with their twins; cases with issues per check: {counts}")
    for entry in report['crashed']:
        faults = '; '.join(c['fault'] for c in entry.get('crash_logs', []) if c['fault'])
        print(f"  CRASH {entry['case']}: {entry['what']}{' at ' + faults if faults else ''}")
    for name, result in h2['cases'].items():
        for check, items in result['issues'].items():
            item = items[0]
            print(f"  H/2 {name} [{result['scope']}] {check} x{result['counts'][check]}: {item['message']} "
                  f"z={item['z']} xy={item['xy']} {json.dumps(item['measured'])}")
    failed = (report['crashed'] or report['slicing_error'] or report['timeout'] or report['memory_cap'] or
              report['mod_not_loaded'] or report['gcode_problems'] or h2['counts'] or report['nondeterministic'])
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
