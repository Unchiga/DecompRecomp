#!/usr/bin/env python3
"""Check AI traces written by MEMORIES_AI_TRACE (src/pc/platform/ai_trace.c).

    python tools/pc/ai_trace_check.py TRACE... [--exe tmp/pc/game32/memories-pc.exe]
                                      [--listing tmp/ai-scripts]

A trace has one line per frame (F), per VSync(1) made by AiScript_Run (V: the
command's script offset, the script base and the opponent id) and per rand()
(R: the caller's address). A decision starts at a command at offset 0
(AiScript_Init resets the cursor) and runs to the next one.

Reported:
  * decisions, commands and frames per decision;
  * every rand() caller, and the ones that drew in a frame between a
    decision's first and last command: anything but the AI's own handlers
    there would move the AI's later rolls with the number of frames it takes;
  * with --listing (the output directory of the upstream
    tools/project/ai_script_disasm.py): logged offsets that are not an
    instruction start of hand_full.txt / field_full.txt.
"""
import argparse
import bisect
import collections
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
BASES = {'801A8000': 'hand', '801A9800': 'field'}


def load_symbols(exe):
    nm = shutil.which('llvm-nm')
    clang = shutil.which('i686-w64-mingw32-clang')
    if not nm and clang:
        nm = os.path.join(os.path.dirname(clang), 'llvm-nm')
    out = subprocess.run([nm or 'llvm-nm', '-n', '--defined-only', exe], capture_output=True, text=True,
                         check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'tT':
            addrs.append(int(parts[0], 16))
            names.append(parts[2].lstrip('_'))
    return addrs, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('traces', nargs='+')
    ap.add_argument('--exe', default=os.path.join(ROOT, 'tmp', 'pc', 'game32', 'memories-pc.exe'))
    ap.add_argument('--listing')
    args = ap.parse_args()
    addrs, names = load_symbols(args.exe)

    decisions, callers, inside = [], collections.Counter(), collections.Counter()
    offsets = collections.defaultdict(set)
    for path in args.traces:
        slide, frame, cur, draws = 0, 0, None, []
        per_trace = []
        for line in open(path):
            p = line.split()
            if p[0] == 'A':
                slide = int(p[1], 16) - addrs[names.index('Memories_Rand')]
            elif p[0] == 'F':
                frame = int(p[1])
            elif p[0] == 'V':
                off, base = int(p[1]), p[2].upper().lstrip('0')
                offsets[base].add(off)
                if off == 0 or cur is None or cur['base'] != base:
                    cur = {'base': base, 'opp': int(p[3]), 'first': frame, 'last': frame, 'commands': 0}
                    per_trace.append(cur)
                cur['last'] = frame
                cur['commands'] += 1
            elif p[0] == 'R':
                i = bisect.bisect_right(addrs, int(p[1], 16) - slide) - 1
                draws.append((frame, names[i] if i >= 0 else '?'))
        busy = set()
        for d in per_trace:
            busy.update(range(d['first'], d['last'] + 1))
        for f, who in draws:
            callers[who] += 1
            if f in busy:
                inside[who] += 1
        decisions += per_trace

    spans = [d['last'] - d['first'] + 1 for d in decisions]
    print('decisions %d (hand %d, field %d), opponents %s' % (
        len(decisions), sum(BASES.get(d['base']) == 'hand' for d in decisions),
        sum(BASES.get(d['base']) == 'field' for d in decisions), sorted({d['opp'] for d in decisions})))
    if decisions:
        print('commands %d; frames per decision: max %d, mean %.1f' % (
            sum(d['commands'] for d in decisions), max(spans), sum(spans) / len(spans)))
    print('rand() callers:')
    for who, n in callers.most_common():
        print('  %8d  %s' % (n, who))
    print('rand() callers in frames where a decision was in progress:')
    for who, n in inside.most_common():
        print('  %8d  %s%s' % (n, who, '' if who.startswith('AiScript_') else '   <- not the AI'))
    outside = sum(n for who, n in inside.items() if not who.startswith('AiScript_'))
    print('draws by other code during a decision: %d' % outside)
    bad = 0
    if args.listing:
        for base, name in BASES.items():
            starts = set()
            for line in open(os.path.join(args.listing, name + '_full.txt')):
                m = re.match(r'\s+801A[0-9A-F]{4} ([0-9A-F]{4})  ', line)
                if m:
                    starts.add(int(m.group(1), 16))
            wrong = offsets[base] - starts
            bad += len(wrong)
            print('%s: %d of %d instruction starts reached, %d logged offsets off an instruction start%s' % (
                name, len(offsets[base] & starts), len(starts), len(wrong),
                (': ' + ' '.join('%04X' % o for o in sorted(wrong)[:10])) if wrong else ''))
    return 1 if outside or bad else 0


if __name__ == '__main__':
    sys.exit(main())
