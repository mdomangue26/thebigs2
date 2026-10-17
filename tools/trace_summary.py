#!/usr/bin/env python3
"""Summarise render traces (docs/native-renderer.md, phase 0).

  trace_summary.py [--names docs/d3d-names.txt] [--range d3d|renderlib]
                   label=render_trace_NNN.txt [label=...]

For the chosen range (default d3d), lists every function called from outside
it - the API surface the rest of the game uses - with calls per trace and the
calling functions (return addresses mapped to the containing function, from
the generated code). Internal calls (range -> same range) are left out.
"""
import argparse, bisect, glob, os, re, sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RANGES = {  # same spans as the TB2_RENDER_TRACE block in CMakeLists.txt
    'd3d': [(0x823A6400, 0x823DE000), (0x824D92E8, 0x824DC3E8)],
    'renderlib': [(0x82460000, 0x8249B800)],
}


def in_range(name, a):
    return any(lo <= a < hi for lo, hi in RANGES[name])


def function_starts():
    cache = os.path.join(ROOT, 'out', 'function_starts.txt')
    gen = os.path.join(ROOT, 'generated', 'default')
    stamp = os.path.join(gen, 'codegen.build.stamp')
    if os.path.exists(cache) and (not os.path.exists(stamp) or
                                  os.path.getmtime(cache) >= os.path.getmtime(stamp)):
        return [int(x, 16) for x in open(cache).read().split()]
    func_re = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', re.M)
    starts = set()
    for path in glob.glob(f'{gen}/*_recomp.*.cpp'):
        starts |= {int(a, 16) for a in func_re.findall(open(path, errors='replace').read())}
    starts = sorted(starts)
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    with open(cache, 'w') as f:
        f.write('\n'.join(f'{a:08X}' for a in starts))
    return starts


def load_names(path):
    names = {}
    if path and os.path.exists(path):
        for line in open(path):
            line = line.split('#', 1)[0].strip()
            if line:
                addr, _, name = line.partition(' ')
                names[int(addr, 16)] = name.strip()
    return names


def read_calls(path):
    """(function, return address) -> count, from the functions section."""
    calls = Counter()
    section = None
    for line in open(path):
        if line.startswith('## '):
            section = line[3:12]
            continue
        if section != 'functions' or not line.strip():
            continue
        parts = line.split()
        fn = int(parts[1], 16)
        for site in parts[3:]:
            if 'x' in site and not site.startswith('('):
                ra, n = site.split('x')
                calls[(fn, int(ra, 16))] += int(n)
        # call sites beyond the 12 listed are summed into the remainder
        listed = sum(int(s.split('x')[1]) for s in parts[3:] if 'x' in s and not s.startswith('('))
        if int(parts[2]) > listed:
            calls[(fn, 0)] += int(parts[2]) - listed
    return calls


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--names', default=os.path.join(ROOT, 'docs', 'd3d-names.txt'))
    ap.add_argument('--range', default='d3d', choices=RANGES)
    ap.add_argument('traces', nargs='+')
    args = ap.parse_args()

    starts = function_starts()
    names = load_names(args.names)

    def owner(ra):
        i = bisect.bisect_right(starts, ra) - 1
        return starts[i] if i >= 0 else 0

    def label(a):
        n = names.get(a)
        return f'{a:08X} {n}' if n else f'{a:08X}'

    def area(a):
        for name in RANGES:
            if in_range(name, a):
                return name
        return 'game'

    labels, per_trace = [], []
    callers = defaultdict(Counter)
    for spec in args.traces:
        lab, _, path = spec.rpartition('=')
        labels.append(lab or os.path.basename(path))
        counts = Counter()
        for (fn, ra), n in read_calls(path).items():
            if not in_range(args.range, fn):
                continue
            if ra == 0:  # unlisted sites: assume external only if all listed ones are
                continue
            caller = owner(ra)
            if in_range(args.range, caller):
                continue
            counts[fn] += n
            callers[fn][caller] += n
        per_trace.append(counts)

    fns = sorted(set().union(*per_trace), key=lambda f: -max(c[f] for c in per_trace))
    w = max(7, *(len(l) for l in labels))
    print(f'# {args.range} functions called from outside the range: {len(fns)}')
    print(f'{"function":<40}' + ''.join(f'{l:>{w + 1}}' for l in labels) + '  callers')
    for fn in fns:
        cs = callers[fn].most_common()
        areas = Counter()
        for c, n in cs:
            areas[area(c)] += n
        who = ' '.join(f'{label(c)}' for c, _ in cs[:4]) + (f' (+{len(cs) - 4})' if len(cs) > 4 else '')
        print(f'{label(fn):<40}' + ''.join(f'{c[fn]:>{w + 1}}' for c in per_trace) +
              f'  [{",".join(sorted(areas))}] {who}')


if __name__ == '__main__':
    main()
