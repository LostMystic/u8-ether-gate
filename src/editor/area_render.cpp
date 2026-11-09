// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "editor/area_render.h"

#include <algorithm>
#include <climits>

#include "data/gamedata.h"
#include "gfx/surface.h"
#include "world/current_map.h"
#include "world/world_view.h"

namespace pg {

std::vector<uint16_t> place_items(CurrentMap& map, const std::vector<ItemRecord>& items, int ox, int oy, int oz) {
    std::vector<uint16_t> ids;
    ids.reserve(items.size());
    for (const ItemRecord& r0 : items) {
        ItemRecord r = r0;
        int x = r.x + ox, y = r.y + oy, z = r.z + oz;
        if (x < 0 || y < 0 || x >= 32768 || y >= 32768 || z < 0 || z > 255) continue;
        r.x = uint16_t(x);
        r.y = uint16_t(y);
        r.z = uint8_t(z);
        uint16_t id = map.create_item(r, Item::kFixed);
        if (!id) continue;
        map.link(id);
        ids.push_back(id);
    }
    for (uint16_t id : ids) {
        const Item& it = map.item(id);
        map.set_fast(it.x / CurrentMap::kChunkSize, it.y / CurrentMap::kChunkSize);
    }
    return ids;
}

namespace {
// Renders whatever is fast in `map`, framing the items `ids`.
AreaImage render_ids(const GameData& data, CurrentMap& map, const std::vector<uint16_t>& ids, const AreaRenderOptions& opt) {
    AreaImage img;
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (uint16_t id : ids) {
        const Item& it = map.item(id);
        const ShapeInfo& si = data.typeflags.get(it.shape);
        if (si.is(ShapeInfo::kEditor) && !opt.show_editor) continue;
        const ShapeFrame* f = data.shapes.frame(it.shape, it.frame);
        if (!f) continue;
        int sx = it.x / 4 - it.y / 4, sy = it.x / 8 + it.y / 8 - it.z;
        x0 = std::min(x0, sx - std::max(f->xoff, f->w - f->xoff));
        x1 = std::max(x1, sx + std::max(f->xoff, f->w - f->xoff));
        y0 = std::min(y0, sy - f->yoff);
        y1 = std::max(y1, sy - f->yoff + f->h);
    }
    if (x0 > x1) { x0 = y0 = 0; x1 = y1 = 64; }
    int W = std::min(opt.max_w, x1 - x0 + 2 * opt.margin), H = std::min(opt.max_h, y1 - y0 + 2 * opt.margin);
    W = std::max(W, 16) & ~1;
    H = std::max(H, 16) & ~1;
    Surface8 surf(W, H);
    surf.clear(0);
    WorldView view(data);
    WorldView::Options vo;
    vo.roof_hiding = false;
    vo.show_editor = opt.show_editor;
    vo.use_screen_camera = true;
    vo.camera_sx = (x0 + x1) / 2;
    vo.camera_sy = (y0 + y1) / 2;
    int cam[3] = {2 * vo.camera_sx + 4 * vo.camera_sy, 4 * vo.camera_sy - 2 * vo.camera_sx, 0};
    view.render(map, surf, cam, cam, vo);
    surf.to_rgba(data.palette, PaletteTransform{}, img.rgba);
    img.w = W;
    img.h = H;
    return img;
}
}  // namespace

AreaImage render_items(const GameData& data, const std::vector<ItemRecord>& items, const AreaRenderOptions& opt, int ox,
                       int oy, int oz) {
    CurrentMap map(data);
    map.reset(255);
    std::vector<uint16_t> ids = place_items(map, items, ox, oy, oz);
    return render_ids(data, map, ids, opt);
}

AreaImage render_map_region(const GameData& data, unsigned mapnum, int x0, int y0, int x1, int y1,
                            const AreaRenderOptions& opt) {
    CurrentMap map(data);
    map.load(mapnum);
    for (int cy = std::max(0, y0 / 512); cy <= std::min(63, (y1 + 511) / 512); ++cy)
        for (int cx = std::max(0, x0 / 512); cx <= std::min(63, (x1 + 511) / 512); ++cx) map.set_fast(cx, cy);
    std::vector<uint16_t> keep;
    for (int cy = 0; cy < 64; ++cy)
        for (int cx = 0; cx < 64; ++cx) {
            std::vector<uint16_t> ids = map.chunk(cx, cy);
            for (uint16_t id : ids) {
                const Item& it = map.item(id);
                if (it.x >= x0 && it.x < x1 && it.y >= y0 && it.y < y1) keep.push_back(id);
                else map.unlink(id);
            }
        }
    return render_ids(data, map, keep, opt);
}

}  // namespace pg
