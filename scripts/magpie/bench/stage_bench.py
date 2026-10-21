#!/usr/bin/env python3
"""Per-function slicing time, for finding where slicing spends its time.

Needs a build configured with -DMAGPIE_SLICING_TIMING=ON. Each case is sliced `--repeat` times
through the golden harness with MAGPIE_STAGE_TIMING_FILE set; the CLI writes, at exit, the summed
time and call count of every timed scope (src/libslic3r/StageTimer.hpp). The report gives the
median over the repeats.

  step.*  wall time of a Print step (steps never overlap themselves)
  fn.*    time inside one function, summed over threads: compare it with its step to see how
          much of the step a faster implementation could remove

    python stage_bench.py --cli <timing build CLI> --datadir <golden datadir> --out <dir> [--repeat 3]
"""
import argparse
import json
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN = os.path.join(os.path.dirname(HERE), 'golden')
sys.path.insert(0, GOLDEN)
import golden  # noqa: E402

BUNNY = 'resources/handy_models/Stanford_Bunny.drc'
BENCHY = 'resources/handy_models/3DBenchy.drc'
FROG = 'tests/data/frog_legs.obj'


def cases():
    base = {'printer': 'u1'}
    def case(name, model, process, scale=None):
        c = dict(base, name=name, model=model, process=process)
        if scale:
            c['cli_args'] = ['--scale', str(scale)]
        return c
    support = {'enable_support': '1', 'support_threshold_angle': '40'}
    return [
        case('benchy_default', BENCHY, {}),
        case('benchy_gyroid', BENCHY, {'sparse_infill_pattern': 'gyroid', 'sparse_infill_density': '20%'}),
        case('benchy_lightning', BENCHY, {'sparse_infill_pattern': 'lightning', 'sparse_infill_density': '20%'}),
        case('benchy_arachne', BENCHY, {'wall_generator': 'arachne'}),
        case('benchy_classic', BENCHY, {'wall_generator': 'classic'}),
        case('bunny_normal_support', BUNNY, dict(support, support_type='normal(auto)')),
        case('bunny_tree_organic', BUNNY, dict(support, support_type='tree(auto)', support_style='organic')),
        case('bunny_tree_slim', BUNNY, dict(support, support_type='tree(auto)', support_style='tree_slim')),
        case('bunny_x2_gyroid', BUNNY, {'sparse_infill_pattern': 'gyroid', 'sparse_infill_density': '15%'}, scale=2),
        case('frog_rectilinear', FROG, {'sparse_infill_pattern': 'rectilinear', 'sparse_infill_density': '40%'}),
        # Heavy jobs.
        case('benchy_x3_fine_gyroid', BENCHY, {'layer_height': '0.08', 'sparse_infill_pattern': 'gyroid',
                                               'sparse_infill_density': '25%'}, scale=3),
        case('bunny_x2_fine_tree', BUNNY, dict(support, support_type='tree(auto)', support_style='organic',
                                               layer_height='0.08'), scale=2),
    ]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cli', required=True)
    p.add_argument('--datadir', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--repeat', type=int, default=3)
    p.add_argument('--cases', help='comma-separated case names')
    a = p.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    corpus = {
        'printers': {'u1': {'machine': 'presets/u1_04_machine.json', 'process': 'presets/u1_04_process.json',
                            'filaments': ['presets/u1_pla.json'], 'bed_type': 'High Temp Plate'}},
        'cases': cases(),
    }
    corpus_path = os.path.join(golden.HERE, '_stage_bench.json')
    with open(corpus_path, 'w', encoding='utf-8') as f:
        json.dump(corpus, f, indent=1)
    wanted = set(a.cases.split(',')) if a.cases else None
    report = {}
    try:
        for c in corpus['cases']:
            if wanted and c['name'] not in wanted:
                continue
            runs = []
            for r in range(a.repeat):
                timing = os.path.join(out, f"{c['name']}.{r}.tsv")
                os.environ['MAGPIE_STAGE_TIMING_FILE'] = timing
                if os.path.exists(timing):
                    os.remove(timing)
                slices = os.path.join(out, 'slices')
                if golden.slice_corpus(a.cli, slices, corpus_path, None, {c['name']}, [], 1800,
                                       os.path.abspath(a.datadir)) != 0:
                    sys.exit(f"{c['name']}: slice failed")
                with open(os.path.join(slices, 'manifest.json'), encoding='utf-8') as f:
                    # Whole CLI process: preset loading, slicing, export and exit.
                    totals = {'cli.wall': (json.load(f)['cases'][c['name']]['seconds'], 1)}
                if os.path.exists(timing):
                    for line in open(timing, encoding='utf-8'):
                        name, seconds, calls = line.rstrip('\n').split('\t')
                        totals[name] = (float(seconds), int(calls))
                runs.append(totals)
            names = sorted(set().union(*[set(r) for r in runs]))
            report[c['name']] = {n: {'seconds': statistics.median(r.get(n, (0., 0))[0] for r in runs),
                                     'calls': runs[0].get(n, (0., 0))[1]} for n in names}
            print(c['name'], {n: round(v['seconds'], 3) for n, v in report[c['name']].items()})
    finally:
        os.environ.pop('MAGPIE_STAGE_TIMING_FILE', None)
        os.remove(corpus_path)
    with open(os.path.join(out, 'report.json'), 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=1)


if __name__ == '__main__':
    main()
