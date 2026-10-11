"""Offline check of KYTY_DRAW_SNAPSHOT folders (vertex explosions as the first case).

python draw_snapshot_check.py <_draw_snapshots dir or one snapshot dir> [--huge 1e5]

For each snapshot: the index range (restart index left out) against every guest vertex buffer
the fetch reads (records = vertices when the stride is not 0), and a float32 scan of each host
vertex buffer and each stage buffer for NaN/Inf and values above --huge. Prints one block per
snapshot, flagged lines start with '!!'.
"""
import json
import math
import os
import struct
import sys


def load_blob(root, entry):
    name = entry.get('blob') if isinstance(entry, dict) else None
    if not name:
        return None
    path = os.path.join(root, 'blobs', name + '.bin')
    if not os.path.exists(path):
        return None
    with open(path, 'rb') as f:
        return f.read()


def float_scan(data, huge):
    n = len(data) // 4
    bad = big = 0
    first = None
    for i, (v,) in enumerate(struct.iter_unpack('<f', data[:n * 4])):
        if math.isnan(v) or math.isinf(v):
            bad += 1
            first = first if first is not None else i
        elif abs(v) > huge:
            big += 1
            first = first if first is not None else i
    return n, bad, big, first


def check(folder, root, huge):
    with open(os.path.join(folder, 'meta.json'), encoding='utf-8') as f:
        meta = json.load(f)
    d = meta['draw']
    print(f"== {os.path.basename(folder)}  vs={meta['vs']} ps={meta['ps']} frame={meta['frame']} "
          f"prim={d['primitive']} indexed={d['indexed']} gpu_args={d['gpu_args']} count={d['index_count']} "
          f"inst={d['instance_count']} voff={d['vertex_offset']} first={d['first_vertex']}")
    hi = None
    if d['indexed']:
        ib = meta['index_buffer']
        data = load_blob(root, ib)
        if data is None:
            print(f"   index buffer not captured: {ib.get('skipped', ib)}")
        else:
            size = ib['host_index_size']
            fmt = '<H' if size == 2 else '<I'
            restart = 0xFFFF if size == 2 else 0xFFFFFFFF
            idx = [v for (v,) in struct.iter_unpack(fmt, data[:len(data) // size * size])]
            used = [v for v in idx if not (d['restart'] and v == restart)]
            if used:
                lo, hi = min(used), max(used)
                print(f"   indices: {len(idx)} (restart {len(idx) - len(used)}), min {lo} max {hi}")
            else:
                print("   indices: none")
    else:
        hi = d['first_vertex'] + max(d['index_count'], 1) - 1
    if hi is not None:
        last = hi + d['vertex_offset'] if d['indexed'] else hi
        for i, b in enumerate(meta['fetch']['buffers']):
            records = b['num_records']
            flag = '!!' if b['stride'] and last >= records else '  '
            print(f" {flag} fetch buffer {i}: addr=0x{b['address']:x} stride={b['stride']} records={records} "
                  f"last vertex read={last}")
    for i, vb in enumerate(meta['vertex_buffers']):
        data = load_blob(root, vb)
        if data is None:
            continue
        n, bad, big, first = float_scan(data, huge)
        flag = '!!' if bad or big else '  '
        print(f" {flag} host vertex buffer {i}: {n} floats, NaN/Inf {bad}, >{huge:g} {big}"
              + (f", first at float {first}" if first is not None else ''))
    for st in meta['stages']:
        for b in st['buffers']:
            data = load_blob(root, b)
            if data is None:
                continue
            n, bad, big, first = float_scan(data, huge)
            if bad or big:
                print(f" !! {st['kind']} {st['shader']} buffer {b['index']} addr=0x{b['address']:x}: "
                      f"NaN/Inf {bad}, >{huge:g} {big} of {n} floats (first at {first})")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    path = sys.argv[1]
    huge = float(sys.argv[sys.argv.index('--huge') + 1]) if '--huge' in sys.argv else 1e5
    if os.path.exists(os.path.join(path, 'meta.json')):
        check(path, os.path.dirname(path), huge)
        return
    for name in sorted(os.listdir(path)):
        folder = os.path.join(path, name)
        if os.path.exists(os.path.join(folder, 'meta.json')):
            check(folder, path, huge)


if __name__ == '__main__':
    main()
