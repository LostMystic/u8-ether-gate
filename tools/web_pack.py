#!/usr/bin/env python3
# Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
"""Packs the web build's data for web/packs.js and writes version.json.

    web_pack.py OUT_DIR GAME_DIR DATA_DIR CHANGELOG

GAME_DIR "-": a build for any host. No game file is packed, and version.json says "public": the
page asks the player for their own game folder (web/packs.js PGGame).

OUT_DIR is the web build folder (build-web/web: pagan.wasm, pagan.js, pagan.html are already
there). Writes OUT_DIR/files/<name>.<hash>.<i>.part and OUT_DIR/version.json. Every pack, and
the program (pagan.wasm, as pack "program"), is cut into chunks of 512 KB, each its own file:
the browser downloads and keeps them one by one, so a slow or broken connection loses at most
the chunk in flight (nginx serves the .gz copies whole, so HTTP ranges can't be relied on).

The game folder (STATIC GAMEDAT USECODE SOUND SAVEGAME, mounted at /game/...) and the engine's
data folder (mounted at /data/...) are grouped into packs by what they are (shapes, maps,
usecode, sound, music, movies, the debug menu's story checkpoints, ...). Every file of those folders goes into some pack, so a new
file in engine/data (for example a JSON table) is shipped without touching this script. A
pack's name carries the hash of its bytes, so an unchanged pack keeps its name and a browser
that has it does not download it again. The program (pagan.wasm) is handled the same way.

Pack format: b"PGPK", uint32 LE length of a JSON index, the index ([[path, offset, size], ...],
offsets counted from the end of the index), then the files' bytes.

version.json: version (the top numbered version of CHANGELOG.md), date, built, build (a hash
of all packs and the page files: changes on every deploy that changes anything), packs
[{name, kind, hash, size, chunks: [{file, size, sha1}]}], notes (the current version's), unreleased (the changelog's
Unreleased section) and history (earlier versions: [{version, date, notes}]).
"""
import hashlib
import json
import os
import re
import sys
import time

GAME_DIRS = ['STATIC', 'GAMEDAT', 'USECODE', 'SOUND', 'SAVEGAME']
UI_FILES = {'U8FONTS.FLX', 'U8GUMPS.FLX', 'U8MOUSE.SHP', 'GUMPAGE.DAT', 'WPNOVLAY.DAT', 'QUOTES.DAT',
            'ECREDITS.DAT'}
MAP_FILES = {'FIXED.DAT', 'GLOB.FLX'}


def group_of(mount):
    """The pack a file (by its path in the virtual file system) belongs to."""
    parts = mount.strip('/').split('/')
    name = parts[-1].upper()
    if parts[0] == 'data':
        if len(parts) > 2 and parts[1] == 'story':
            return 'story'  # the debug menu's story checkpoints (game/story_pack.h): their own pack
        return 'music' if len(parts) > 2 and parts[1] == 'soundfont' else 'engine-data'
    top = parts[1].upper() if len(parts) > 1 else ''
    if top == 'STATIC':
        if name == 'U8SHAPES.FLX':
            return 'shapes'
        if name.endswith('.SKF'):
            return 'movies'
        if name in UI_FILES:
            return 'fonts-gumps'
        if name in MAP_FILES:
            return 'maps'
        return 'static'
    if top in ('GAMEDAT', 'SAVEGAME'):
        return 'maps'
    return top.lower() or 'misc'


def files_under(folder, mount):
    out = []
    if not os.path.isdir(folder):
        return out
    for dirpath, dirnames, filenames in os.walk(folder, followlinks=True):
        dirnames.sort()
        for f in sorted(filenames):
            full = os.path.join(dirpath, f)
            rel = os.path.relpath(full, folder).replace(os.sep, '/')
            out.append((mount + '/' + rel, full))
    return out


def make_pack(entries):
    index, blobs, at = [], [], 0
    for path, full in entries:
        with open(full, 'rb') as f:
            data = f.read()
        index.append([path, at, len(data)])
        blobs.append(data)
        at += len(data)
    head = json.dumps(index, separators=(',', ':')).encode()
    return b'PGPK' + len(head).to_bytes(4, 'little') + head + b''.join(blobs)


CHUNK = 512 * 1024


def write_chunks(files_dir, name, kind, blob):
    """Writes blob as files/<name>.<hash>.<i>.part of CHUNK bytes; returns its version.json entry."""
    h = hashlib.sha1(blob).hexdigest()[:12]
    chunks = []
    for i, at in enumerate(range(0, max(1, len(blob)), CHUNK)):
        part = blob[at:at + CHUNK]
        file = '%s.%s.%d.part' % (name, h, i)
        with open(os.path.join(files_dir, file), 'wb') as f:
            f.write(part)
        chunks.append({'file': file, 'size': len(part), 'sha1': hashlib.sha1(part).hexdigest()})
    return {'name': name, 'kind': kind, 'hash': h, 'size': len(blob), 'chunks': chunks}


def read_changelog(path):
    """[(title, date, [notes])] in file order, title 'Unreleased' or a version number."""
    sections, cur = [], None
    if not os.path.exists(path):
        return sections
    text = open(path, encoding='utf-8').read()
    text = re.sub(r'<!--.*?-->', '', text, flags=re.S)
    for line in text.splitlines():
        m = re.match(r'^##\s+(.+?)\s*$', line)
        if m:
            head = m.group(1)
            hm = re.match(r'^v?(\d+(?:\.\d+)*)\s*(?:[-–—(]\s*(.*?)\)?)?$', head)
            if hm:
                cur = [hm.group(1), (hm.group(2) or '').strip(), []]
            else:
                cur = [head.strip(), '', []]
            sections.append(cur)
            continue
        if cur is None:
            continue
        if re.match(r'^\s*[-*]\s+', line):
            cur[2].append(re.sub(r'^\s*[-*]\s+', '', line).strip())
        elif line.strip() and cur[2] and line[:1].isspace():
            cur[2][-1] += ' ' + line.strip()
    return sections


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    out, game, data, changelog = sys.argv[1:5]
    files_dir = os.path.join(out, 'files')
    os.makedirs(files_dir, exist_ok=True)
    for f in os.listdir(files_dir):
        os.remove(os.path.join(files_dir, f))

    public = game == '-'
    entries = []
    for d in ([] if public else GAME_DIRS):
        entries += files_under(os.path.join(game, d), '/game/' + d)
    # The story checkpoints are saves of the original game's world: never in a public build.
    entries += [e for e in files_under(data, '/data') if not (public and e[0].startswith('/data/story/'))]
    groups = {}
    for path, full in entries:
        groups.setdefault(group_of(path), []).append((path, full))

    build = hashlib.sha1()
    packs = []
    wasm = os.path.join(out, 'pagan.wasm')
    if os.path.exists(wasm):
        blob = open(wasm, 'rb').read()
        packs.append(write_chunks(files_dir, 'program', 'wasm', blob))
        build.update(packs[-1]['hash'].encode())
    for name in sorted(groups):
        blob = make_pack(groups[name])
        packs.append(write_chunks(files_dir, name, 'pack', blob))
        packs[-1]['files'] = len(groups[name])
        build.update(packs[-1]['hash'].encode())
    for extra in ('pagan.js', 'pagan.html', 'exit.html', 'packs.js'):
        p = os.path.join(out, extra)
        if os.path.exists(p):
            build.update(hashlib.sha1(open(p, 'rb').read()).digest())

    sections = read_changelog(changelog)
    unreleased = [s for s in sections if s[0].lower() == 'unreleased']
    numbered = [s for s in sections if re.match(r'^\d', s[0])]
    current = numbered[0] if numbered else ['0.0.0', '', []]
    m = {
        'version': current[0],
        'date': current[1],
        'built': time.strftime('%Y-%m-%d %H:%M'),
        'build': build.hexdigest()[:12],
        'packs': packs,
        'notes': current[2],
        'unreleased': unreleased[0][2] if unreleased else [],
        'history': [{'version': s[0], 'date': s[1], 'notes': s[2]} for s in numbered[1:]],
    }
    if public:
        m['public'] = True
    with open(os.path.join(out, 'version.json'), 'w', encoding='utf-8') as f:
        json.dump(m, f, indent=1, ensure_ascii=False)
    total = sum(p['size'] for p in packs)
    print('web_pack: %d packs, %.1f MB, version %s, build %s' % (len(packs), total / 1e6, m['version'], m['build']))
    for p in packs:
        print('  %-12s %9d  %s.%s (%d chunks)' % (p['name'], p['size'], p['name'], p['hash'], len(p['chunks'])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
