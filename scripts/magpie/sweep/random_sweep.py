#!/usr/bin/env python3
"""Random setting and mod combination sweep (P6; Magpie line 1 checked 204 combinations).

Builds N random cases from a seed: model, support generator and type, mods (half-height walls
and support, low temperature interface and tower, support extras, multi-nozzle walls on a mixed
nozzle U1), wall generator and count, layer height, infill. Slices them with the golden harness,
re-slices every fourth case to check determinism, and checks every G-code:

- the CLI either slices or rejects the settings with a message; a crash or timeout fails;
- XY stays on the bed (with 1 mm slack), Z stays between 0 and the bed height;
- nozzle and bed temperatures stay within 0-350 degC;
- no NaN or infinite numbers;
- extrusion amounts (relative E) per move stay below 50 mm of filament.

    python random_sweep.py --cli <magpie-slicer-next.exe> --datadir <golden datadir> \
        --external-models <dir with tee.stl> --out <dir> --count 204 --seed 1
"""
import argparse
import json
import math
import os
import random
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'golden'))
import golden  # noqa: E402

MODELS = ['tests/data/20mm_cube.obj', 'tests/data/overhang.obj', 'tests/data/frog_legs.obj', 'external:tee.stl']
BED_MM = 270.0         # U1 printable area, mm
BED_SLACK_MM = 1.0     # moves may touch the edge by this much, mm
MAX_TEMP_C = 350.0     # highest believable nozzle or bed temperature, degC
MAX_E_PER_MOVE_MM = 50.0  # filament per single move, mm


def random_case(rng, index):
    multi = rng.random() < 0.2
    support = rng.choice(['off', 'normal(auto)', 'tree(auto)'])
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
    if rng.random() < 0.3:
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
    case = {'name': f'case_{index:03d}', 'model': rng.choice(MODELS), 'process': process}
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


def build_corpus(seed, count):
    rng = random.Random(seed)
    return {
        'description': f'Random sweep, seed {seed}, {count} cases',
        'printers': {
            'u1': {'machine': 'presets/u1_04_machine.json', 'process': 'presets/u1_04_process.json',
                   'filaments': ['presets/u1_pla.json'], 'bed_type': 'High Temp Plate'},
            'u1mn': {'machine': 'presets/u1_04_machine.json', 'process': 'presets/u1_04_process.json',
                     'filaments': ['presets/u1_pla.json', 'presets/u1_pla_2.json', 'presets/u1_pla_3.json', 'presets/u1_pla_4.json'],
                     'bed_type': 'High Temp Plate'},
        },
        'cases': [random_case(rng, i) for i in range(count)],
    }


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


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cli', required=True)
    p.add_argument('--datadir', required=True)
    p.add_argument('--external-models')
    p.add_argument('--out', required=True)
    p.add_argument('--count', type=int, default=204)
    p.add_argument('--seed', type=int, default=1)
    p.add_argument('--timeout', type=int, default=900)
    p.add_argument('--check-only', action='store_true', help='check the G-code of an earlier run in --out')
    a = p.parse_args()

    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    corpus = build_corpus(a.seed, a.count)
    # The harness resolves presets relative to its own folder; keep the corpus beside it.
    corpus_path = os.path.join(golden.HERE, f'_sweep_{a.seed}.json')
    with open(corpus_path, 'w', encoding='utf-8') as f:
        json.dump(corpus, f, indent=1)
    with open(os.path.join(out, 'corpus.json'), 'w', encoding='utf-8') as f:
        json.dump(corpus, f, indent=1)
    datadir = os.path.abspath(a.datadir)
    repeat = {c['name'] for i, c in enumerate(corpus['cases']) if i % 4 == 0}
    try:
        if a.check_only:
            raise FileExistsError
        golden.slice_corpus(a.cli, os.path.join(out, 'run1'), corpus_path, a.external_models, None, [], a.timeout, datadir)
        golden.slice_corpus(a.cli, os.path.join(out, 'run2'), corpus_path, a.external_models, repeat, [], a.timeout, datadir)
    except FileExistsError:
        pass
    finally:
        os.remove(corpus_path)

    with open(os.path.join(out, 'run1', 'manifest.json'), encoding='utf-8') as f:
        run1 = json.load(f)['cases']
    with open(os.path.join(out, 'run2', 'manifest.json'), encoding='utf-8') as f:
        run2 = json.load(f)['cases']
    report = {'sliced': [], 'rejected': [], 'crashed': [], 'gcode_problems': {}, 'nondeterministic': []}
    for name, r in run1.items():
        if r['status'] == 'ok':
            report['sliced'].append(name)
            gdir = os.path.join(out, 'run1', name, 'out')
            for g in sorted(os.listdir(gdir)):
                if g.endswith('.gcode'):
                    problems = check_gcode(os.path.join(gdir, g))
                    if problems:
                        report['gcode_problems'][name] = problems
            if name in run2 and run2[name].get('sha256') != r['sha256']:
                report['nondeterministic'].append(name)
        else:
            code = r.get('exit')
            # Orca's CLI returns small codes for settings it rejects; anything else is a crash.
            if isinstance(code, int) and 0 < code < 256:
                log = open(os.path.join(out, 'run1', name, 'cli.log'), encoding='utf-8', errors='replace').read()
                report['rejected'].append({'case': name, 'exit': code, 'log_tail': log.strip().splitlines()[-3:]})
            else:
                report['crashed'].append({'case': name, 'exit': code})
    with open(os.path.join(out, 'report.json'), 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=1)
    print(f"sliced {len(report['sliced'])}, rejected {len(report['rejected'])}, crashed {len(report['crashed'])}, "
          f"G-code problems {len(report['gcode_problems'])}, nondeterministic {len(report['nondeterministic'])} "
          f"(of {len(repeat)} repeated)")
    return 1 if report['crashed'] or report['gcode_problems'] or report['nondeterministic'] else 0


if __name__ == '__main__':
    sys.exit(main())
