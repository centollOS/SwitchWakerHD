#!/usr/bin/env python3
"""The NPC catalogue on the player's machine: names and spots from the player's own game files.

The port keeps only a recipe per place (runtime/assets/npc_catalogue/<place>.recipe.json): neutral ids,
our own descriptions, and references into the game (actor class and code, stage, room, layers, the
figurine message number). This tool reads the player's game locally and writes what the recipe leaves
out, never into the repository:
  - the name: the Nintendo Gallery figurine name, message 0x37DD + figure (daObjFigure_c::getMsg), the
    message's first line (only for entries whose figurine is that one character: "own_name");
  - the spot: where the character stands outside (by day if it is out by day), from the room actor
    lists (.dzr in the stage archives).
The port runs it the first time a story mod names a resident (story_npc_catalogue.h) and keeps the
result in its data folder; modders use --list to see which id is which character in their game.

usage: npc_catalogue.py --game <game dir with content/> [--recipe file ...] [--out catalogue.json] [--list] [--json]
"""
import argparse, glob, json, os, re, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import shaderprep  # noqa: E402  (Yaz0 and SARC readers)

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_RECIPES = os.path.join(HERE, '..', '..', 'runtime', 'assets', 'npc_catalogue')


def msbt(d):
    """MSBT label -> text (control codes dropped)"""
    e = '>' if d[8:10] == b'\xfe\xff' else '<'
    nsec = struct.unpack(e + 'H', d[0xE:0x10])[0]
    o = 0x20
    secs = {}
    for _ in range(nsec):
        mag = d[o:o + 4]
        size = struct.unpack(e + 'I', d[o + 4:o + 8])[0]
        secs[mag] = d[o + 0x10:o + 0x10 + size]
        o = (o + 0x10 + size + 15) & ~15
    lbl = secs[b'LBL1']
    labels = {}
    for i in range(struct.unpack(e + 'I', lbl[:4])[0]):
        cnt, off = struct.unpack(e + 'II', lbl[4 + 8 * i:12 + 8 * i])
        p = off
        for _ in range(cnt):
            n = lbl[p]
            name = lbl[p + 1:p + 1 + n].decode()
            labels[struct.unpack(e + 'I', lbl[p + 1 + n:p + 5 + n])[0]] = name
            p += 5 + n
    txt = secs[b'TXT2']
    m = struct.unpack(e + 'I', txt[:4])[0]
    offs = [struct.unpack(e + 'I', txt[4 + 4 * i:8 + 4 * i])[0] for i in range(m)] + [len(txt)]
    out = {}
    for i in range(m):
        raw = txt[offs[i]:offs[i + 1]]
        s = []
        j = 0
        while j + 1 < len(raw):
            c = struct.unpack(e + 'H', raw[j:j + 2])[0]
            if c == 0x0E:  # control: group, type, size, parameters
                j += 8 + struct.unpack(e + 'H', raw[j + 6:j + 8])[0]
                continue
            if c == 0:
                break
            s.append(chr(c))
            j += 2
        out[labels.get(i, str(i))] = ''.join(s)
    return out


def room_files(f):
    """the .dzr/.dzs files of a room archive (BFRES-wrapped RARC as the HD stage archives store them)"""
    s32 = lambda o: struct.unpack_from('>i', f, o)[0]
    g = 0x20 + 4 * 11
    if s32(g) == 0:
        return
    go = g + s32(g)
    for i in range(1, s32(go + 4) + 1):
        e = go + 8 + 16 * i
        no = e + 8 + s32(e + 8)
        name = f[no:f.index(b'\0', no)].decode()
        do = e + 12 + s32(e + 12)
        off = do + s32(do)
        size = struct.unpack_from('>I', f, do + 4)[0]
        if name.endswith('.dzr') or name.endswith('.dzs'):
            yield f[off:off + size]


def actors_in(stage, room, d, out):
    n = struct.unpack_from('>I', d, 0)[0]
    for i in range(n):
        t, c, o = struct.unpack_from('>4sII', d, 4 + 12 * i)
        t = t.decode(errors='replace')
        if not t.startswith('ACT'):
            continue
        for k in range(c):
            e = o + 0x20 * k
            name = d[e:e + 8].split(b'\0')[0].decode(errors='replace')
            x, _, z = struct.unpack_from('>3f', d, e + 12)
            layer = 'R' if t == 'ACTR' else str(int(t[3], 16))
            out.append((stage, room, layer, name, round(x), round(z)))


def read_game(game, stages):
    base = os.path.join(game, 'content', 'Common')
    msgs = {}
    with open(os.path.join(base, 'Pack', 'permanent_2d_UsEnglish.pack'), 'rb') as fh:
        for name, data in shaderprep.sarc(fh.read()):
            if name.startswith('message'):
                for n2, d2 in shaderprep.walk(data, name):
                    if n2.endswith('.msbt'):
                        msgs.update(msbt(d2))
    acts = []

    def room_archive(stage, label, szs):
        for n2, d2 in shaderprep.walk(szs, label):
            if n2.endswith('.bfres'):
                m = re.search(r'Room(\d+)', n2)
                for dz in room_files(d2):
                    actors_in(stage, int(m.group(1)) if m else -1, dz, acts)
    for f in sorted(glob.glob(os.path.join(base, 'Stage', '*_Room*.szs'))):
        stage = os.path.basename(f).split('_Room')[0]
        if stage in stages:
            with open(f, 'rb') as fh:
                room_archive(stage, os.path.basename(f), fh.read())
    for p in ['szs_permanent0.pack', 'szs_permanent1.pack', 'szs_permanent2.pack', 'first_szs_permanent.pack']:
        path = os.path.join(base, 'Pack', p)
        if not os.path.exists(path):
            continue
        with open(path, 'rb') as fh:
            for name, data in shaderprep.sarc(fh.read()):
                stage = name.split('_Room')[0]
                if '_Room' in name and stage in stages:
                    room_archive(stage, name, data)
    return msgs, acts


def build(game, recipes):
    stages = set()
    for r in recipes:
        for e in r['npcs']:
            for a in e['actors']:
                stages.add(a['stage'])
    msgs, acts = read_game(game, stages)
    out = {}
    for r in recipes:
        for e in r['npcs']:
            item = {}
            number = int(e['figure_message'], 16)
            text = msgs.get('%05d' % number)
            if text:
                name = re.sub(r'\s+', ' ', text.split('\n')[0]).strip()
                item['figurine'] = name  # what the figurine is called (a group's or a pair's name too)
                if e.get('own_name'):
                    item['name'] = name
            spot = None
            for a in e['actors']:
                code = a.get('code') or a.get('actor')
                if a['stage'] != 'sea':
                    continue
                here = [t for t in acts if t[0] == 'sea' and t[1] == a['room'] and t[3] == code and t[2] in a['layers']]
                by_day = [t for t in here if t[2] in ('0', '4', 'R')]
                pick = (by_day or here)
                if pick and spot is None:
                    spot = {'x': pick[0][4], 'z': pick[0][5]}
            if spot:
                item['spot'] = spot
            out[e['id']] = item
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True)
    ap.add_argument('--recipe', action='append', help='recipe files (default: the port\'s)')
    ap.add_argument('--out', help='write the catalogue here (JSON)')
    ap.add_argument('--list', action='store_true', help='print id, name, place and spot')
    ap.add_argument('--json', action='store_true', help='one JSON result line (for the port)')
    args = ap.parse_args()
    files = args.recipe or sorted(glob.glob(os.path.join(DEFAULT_RECIPES, '*.recipe.json')))
    recipes = []
    for f in files:
        with open(f, encoding='utf-8') as fh:
            recipes.append(json.load(fh))
    try:
        npcs = build(args.game, recipes)
    except (OSError, KeyError, struct.error, ValueError) as error:
        if args.json:
            print(json.dumps({'ok': False, 'error': 'NPC catalogue: ' + str(error)}))
            return 1
        raise
    doc = {'catalogue_local': 1, 'npcs': npcs}
    if args.out:
        tmp = args.out + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as fh:
            json.dump(doc, fh, indent=1, ensure_ascii=False)
        os.replace(tmp, args.out)
    if args.list:
        for r in recipes:
            for e in r['npcs']:
                n = npcs.get(e['id'], {})
                spot = n.get('spot')
                print('%-20s %-28s %s%s' % (e['id'], n.get('name') or ('(%s)' % n['figurine'] if 'figurine' in n else '-'), e['where'],
                                            ' at %d, %d' % (spot['x'], spot['z']) if spot else ''))
    if args.json:
        print(json.dumps({'ok': True, 'npcs': len(npcs)}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
