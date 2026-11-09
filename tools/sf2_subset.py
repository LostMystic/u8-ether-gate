#!/usr/bin/env python3
# Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#
# Writes a smaller SoundFont 2 file that keeps only the presets a game's music uses, written
# from the public SoundFont 2.01 specification. Used to cut GeneralUser GS down to the
# General MIDI programs and drum kits of the U8 songs (see engine/data/soundfont/README.md).
#
#   sf2_subset.py IN.sf2 OUT.sf2 --programs 0,1,11,... --kits 0,48 --drum-notes 35,38,...
#                 [--halve-rate-above 30000]
import argparse
import struct
import sys


def chunks(data, off, end):
    while off + 8 <= end:
        tag = data[off:off + 4]
        size = struct.unpack_from('<I', data, off + 4)[0]
        yield tag, off + 8, size
        off += 8 + size + (size & 1)


def parse(data):
    if data[0:4] != b'RIFF' or data[8:12] != b'sfbk':
        sys.exit('not a SoundFont')
    lists = {}
    for tag, off, size in chunks(data, 12, len(data)):
        if tag == b'LIST':
            kind = data[off:off + 4]
            lists[kind] = {t: data[o:o + s] for t, o, s in chunks(data, off + 4, off + size)}
    return lists


def records(blob, size, fmt):
    return [struct.unpack_from(fmt, blob, i) for i in range(0, len(blob) - size + 1, size)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--programs', default='')
    ap.add_argument('--kits', default='0')
    ap.add_argument('--drum-notes', default='')
    ap.add_argument('--halve-rate-above', type=int, default=0)
    a = ap.parse_args()
    progs = {int(x) for x in a.programs.split(',') if x}
    kits = {int(x) for x in a.kits.split(',') if x}
    drum_notes = {int(x) for x in a.drum_notes.split(',') if x}

    data = open(a.src, 'rb').read()
    L = parse(data)
    pd = L[b'pdta']
    smpl = L[b'sdta'][b'smpl']
    phdr = records(pd[b'phdr'], 38, '<20sHHHIII')
    pbag = records(pd[b'pbag'], 4, '<HH')
    pmod = pd[b'pmod']
    pgen = records(pd[b'pgen'], 4, '<HH')
    inst = records(pd[b'inst'], 22, '<20sH')
    ibag = records(pd[b'ibag'], 4, '<HH')
    imod = pd[b'imod']
    igen = records(pd[b'igen'], 4, '<HH')
    shdr = records(pd[b'shdr'], 46, '<20sIIIIIBbHH')

    def wanted(p):
        bank, prog = p[2], p[1]
        return (bank == 0 and prog in progs) or (bank == 128 and prog in kits)

    def key_ok(gens, drum):
        if not drum or not drum_notes:
            return True
        for op, amt in gens:
            if op == 43:
                lo, hi = amt & 0xFF, amt >> 8
                return any(lo <= n <= hi for n in drum_notes)
        return True

    out_phdr, out_pbag, out_pmod, out_pgen = [], [], b'', []
    out_inst, out_ibag, out_imod, out_igen = [], [], b'', []
    inst_map, samp_map = {}, {}
    samp_order = []

    def use_sample(s):
        if s in samp_map:
            return samp_map[s]
        samp_map[s] = len(samp_order)
        samp_order.append(s)
        link = shdr[s][8]
        if shdr[s][9] in (2, 4, 8) and link < len(shdr) - 1:
            use_sample(link)
        return samp_map[s]

    def use_inst(i, drum):
        nonlocal out_imod
        key = (i, drum)
        if key in inst_map:
            return inst_map[key]
        idx = len(out_inst)
        inst_map[key] = idx
        out_inst.append((inst[i][0], len(out_ibag)))
        for b in range(inst[i][1], inst[i + 1][1]):
            gens = igen[ibag[b][0]:ibag[b + 1][0]]
            has_sample = any(op == 53 for op, _ in gens)
            if has_sample and not key_ok(gens, drum):
                continue
            out_ibag.append((len(out_igen), len(out_imod) // 10))
            out_imod += imod[ibag[b][1] * 10:ibag[b + 1][1] * 10]
            for op, amt in gens:
                if op == 53:
                    amt = use_sample(amt)
                out_igen.append((op, amt))
        return idx

    for pi in range(len(phdr) - 1):
        p = phdr[pi]
        if not wanted(p):
            continue
        drum = p[2] == 128
        out_phdr.append((p[0], p[1], p[2], len(out_pbag), p[4], p[5], p[6]))
        for b in range(p[3], phdr[pi + 1][3]):
            gens = pgen[pbag[b][0]:pbag[b + 1][0]]
            has_inst = any(op == 41 for op, _ in gens)
            if has_inst and not key_ok(gens, drum):
                continue
            out_pbag.append((len(out_pgen), len(out_pmod) // 10))
            out_pmod += pmod[pbag[b][1] * 10:pbag[b + 1][1] * 10]
            for op, amt in gens:
                if op == 41:
                    amt = use_inst(amt, drum)
                out_pgen.append((op, amt))

    # Sample data: each sample followed by 46 zero points.
    new_smpl = bytearray()
    out_shdr = []
    for s in samp_order:
        name, start, end, ls, le, rate, pitch, corr, link, typ = shdr[s]
        pcm = smpl[start * 2:end * 2]
        if a.halve_rate_above and rate > a.halve_rate_above:
            vals = struct.unpack('<%dh' % (len(pcm) // 2), pcm)
            half = [(vals[i] + vals[i + 1]) // 2 if i + 1 < len(vals) else vals[i] for i in range(0, len(vals), 2)]
            pcm = struct.pack('<%dh' % len(half), *half)
            ls, le = start + (ls - start) // 2, start + (le - start) // 2
            end = start + len(half)
            rate //= 2
        base = len(new_smpl) // 2
        new_smpl += pcm + b'\0' * 92
        n = len(pcm) // 2
        out_shdr.append((name, base, base + n, base + (ls - start), base + (le - start), rate, pitch, corr,
                         samp_map.get(link, 0), typ))

    def term(lst, rec):
        lst.append(rec)
    term(out_phdr, (b'EOP', 0, 0, len(out_pbag), 0, 0, 0))
    term(out_pbag, (len(out_pgen), len(out_pmod) // 10))
    out_pmod += b'\0' * 10
    term(out_pgen, (0, 0))
    term(out_inst, (b'EOI', len(out_ibag)))
    term(out_ibag, (len(out_igen), len(out_imod) // 10))
    out_imod += b'\0' * 10
    term(out_igen, (0, 0))
    term(out_shdr, (b'EOS', 0, 0, 0, 0, 0, 0, 0, 0, 0))

    def chunk(tag, body):
        return tag + struct.pack('<I', len(body)) + body + (b'\0' if len(body) & 1 else b'')

    def lst(kind, body):
        return chunk(b'LIST', kind + body)

    info = b''.join(chunk(t, v) for t, v in L[b'INFO'].items())
    pdta = (chunk(b'phdr', b''.join(struct.pack('<20sHHHIII', *r) for r in out_phdr)) +
            chunk(b'pbag', b''.join(struct.pack('<HH', *r) for r in out_pbag)) +
            chunk(b'pmod', out_pmod) +
            chunk(b'pgen', b''.join(struct.pack('<HH', *r) for r in out_pgen)) +
            chunk(b'inst', b''.join(struct.pack('<20sH', *r) for r in out_inst)) +
            chunk(b'ibag', b''.join(struct.pack('<HH', *r) for r in out_ibag)) +
            chunk(b'imod', out_imod) +
            chunk(b'igen', b''.join(struct.pack('<HH', *r) for r in out_igen)) +
            chunk(b'shdr', b''.join(struct.pack('<20sIIIIIBbHH', *r) for r in out_shdr)))
    body = b'sfbk' + lst(b'INFO', info) + lst(b'sdta', chunk(b'smpl', bytes(new_smpl))) + lst(b'pdta', pdta)
    open(a.dst, 'wb').write(b'RIFF' + struct.pack('<I', len(body)) + body)
    print('%s: %d presets, %d instruments, %d samples, %d bytes' %
          (a.dst, len(out_phdr) - 1, len(out_inst) - 1, len(out_shdr) - 1, 8 + len(body)))


if __name__ == '__main__':
    main()
