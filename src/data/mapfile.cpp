// Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
// Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "data/mapfile.h"

#include "data/typeflags.h"

namespace pg {

std::vector<ItemRecord> parse_item_records(ByteView d) {
    std::vector<ItemRecord> out;
    out.reserve(d.size / 16);
    for (size_t o = 0; o + 16 <= d.size; o += 16) {
        const uint8_t* p = d.data + o;
        ItemRecord r;
        r.x = rd16(p);
        r.y = rd16(p + 2);
        r.z = p[4];
        r.shape = rd16(p + 5);
        r.frame = p[7];
        r.flags = rd16(p + 8);
        r.quality = rd16(p + 10);
        r.npcnum = p[12];
        r.mapnum = p[13];
        r.next = rd16(p + 14);
        out.push_back(r);
    }
    return out;
}

std::vector<MapEntry> build_item_tree(const std::vector<ItemRecord>& recs, const TypeFlags& tf) {
    std::vector<MapEntry> out;
    out.reserve(recs.size());
    std::vector<int> stack;  // open containers
    for (const ItemRecord& r : recs) {
        while (!stack.empty() && r.x != stack.size()) stack.pop_back();
        MapEntry e;
        e.rec = r;
        e.parent = stack.empty() ? -1 : stack.back();
        out.push_back(e);
        if (tf.get(r.shape).family() == kFamContainer) stack.push_back(int(out.size() - 1));
    }
    return out;
}

bool GlobFile::load(Bytes file) {
    Flex flex;
    if (!flex.load(std::move(file))) return false;
    globs_.assign(flex.count(), {});
    for (unsigned i = 0; i < flex.count(); ++i) {
        ByteView v = flex.entry(i);
        if (v.size < 2) continue;
        unsigned n = rd16(v.data);
        if (2 + size_t(n) * 6 > v.size) return false;
        auto& g = globs_[i];
        g.resize(n);
        for (unsigned k = 0; k < n; ++k) {
            const uint8_t* p = v.data + 2 + 6 * k;
            g[k] = GlobItem{p[0], p[1], p[2], rd16(p + 3), p[5]};
        }
    }
    return true;
}

bool load_npcs(Bytes itemcache, Bytes npcdata, std::vector<NpcRecord>& out) {
    out.clear();
    Flex ic, nd;
    if (!ic.load(std::move(itemcache)) || !nd.load(std::move(npcdata))) return false;
    ByteView c = ic.entry(0), d = nd.entry(0);
    if (c.size < 0x24000 || d.size < 256 * 0x31) return false;
    for (unsigned i = 1; i <= 255; ++i) {
        NpcRecord n;
        n.shape = rd16(c.data + 0x0B400 + 2 * i);
        if (!n.shape) continue;
        n.number = i;
        n.x = rd16(c.data + 2 * i);
        n.y = rd16(c.data + 0x04800 + 2 * i);
        n.z = c.data[0x09000 + i];
        n.flags = rd16(c.data + 0x12000 + 2 * i);
        n.quality = rd16(c.data + 0x16800 + 2 * i);
        n.npcnum = c.data[0x1B000 + i];
        n.mapnum = c.data[0x1D400 + i];
        const uint8_t* r = d.data + 0x31 * i;
        n.frame = uint16_t(c.data[0x0FC00 + i] | (r[0x07] << 8));
        n.str = r[0];
        n.dex = r[1];
        n.intel = r[2];
        n.hp = r[3];
        n.dir = r[4];
        n.last_anim = rd16(r + 5);
        n.alignment = r[0x0B];
        n.unk0c = r[0x0C];
        n.mana = rds16(r + 0x2D);
        n.actor_flags = uint32_t(r[0x1B]) | (uint32_t(r[0x2F]) << 8) | (uint32_t(r[0x30]) << 16);
        out.push_back(n);
    }
    return true;
}

}  // namespace pg
