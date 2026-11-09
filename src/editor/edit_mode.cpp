// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "editor/edit_mode.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>

#include "app/rgba_draw.h"
#include "core/log.h"
#include "data/gamedata.h"
#include "data/palette.h"

// The map dresser (section 3) is used once its library is in the tree.
#if __has_include("editor/dresser.h") && __has_include("editor/dresser.cpp")
#include "editor/area_render.h"
#include "editor/dresser.h"
#include "gfx/surface.h"
#include "json/json.hpp"
#include "world/current_map.h"
#include "world/world_layout.h"
#include "world/world_view.h"
#define PG_HAVE_DRESSER 1
#else
#define PG_HAVE_DRESSER 0
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace pg {

namespace {

constexpr uint32_t kBackground = 0xFF0E1016u;
constexpr uint32_t kBarColor = 0xFF14161Eu;

uint32_t kind_color(char k) {
    switch (k) {
    case cell::kFloor: return 0xFFC8B48Cu;
    case cell::kWall: return 0xFF76665Au;
    case cell::kPassage: return 0xFFE6D8A0u;
    case cell::kDoor: return 0xFFD06A2Cu;
    case cell::kWater: return 0xFF3478C8u;
    case cell::kChasm: return 0xFF24202Cu;
    case cell::kStairs: return 0xFFA8A858u;
    case EditMode::kHeightOnly: return 0xFF9070C0u;
    default: return 0xFF808080u;
    }
}

uint32_t darker(uint32_t c, int pct) {
    uint32_t r = ((c >> 16) & 255) * uint32_t(pct) / 100, g = ((c >> 8) & 255) * uint32_t(pct) / 100, b = (c & 255) * uint32_t(pct) / 100;
    return 0xFF000000u | (std::min(r, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(b, 255u);
}

void blend_px(uint32_t& d, uint32_t color, int alpha) {
    if (alpha >= 255) { d = color | 0xFF000000u; return; }
    uint32_t r = (((d >> 16) & 255) * uint32_t(255 - alpha) + ((color >> 16) & 255) * uint32_t(alpha)) / 255;
    uint32_t g = (((d >> 8) & 255) * uint32_t(255 - alpha) + ((color >> 8) & 255) * uint32_t(alpha)) / 255;
    uint32_t b = ((d & 255) * uint32_t(255 - alpha) + (color & 255) * uint32_t(alpha)) / 255;
    d = 0xFF000000u | (r << 16) | (g << 8) | b;
}

// A convex polygon, filled with alpha.
void fill_poly(const RgbaCanvas& c, const double* xs, const double* ys, int n, uint32_t color, int alpha) {
    double y0 = ys[0], y1 = ys[0];
    for (int i = 1; i < n; ++i) { y0 = std::min(y0, ys[i]); y1 = std::max(y1, ys[i]); }
    int iy0 = std::max(0, int(std::ceil(y0 - 0.5))), iy1 = std::min(c.h - 1, int(std::floor(y1 - 0.5)));
    if (iy0 > iy1 && y1 - y0 < 1 && y0 >= 0 && y0 < c.h) iy0 = iy1 = int(y0);  // thinner than a pixel: one row
    for (int y = iy0; y <= iy1; ++y) {
        double yc = y + 0.5, lo = 1e18, hi = -1e18;
        for (int i = 0; i < n; ++i) {
            double ax = xs[i], ay = ys[i], bx = xs[(i + 1) % n], by = ys[(i + 1) % n];
            if ((ay <= yc && by >= yc) || (by <= yc && ay >= yc)) {
                double x = ay == by ? std::min(ax, bx) : ax + (yc - ay) * (bx - ax) / (by - ay);
                double x2 = ay == by ? std::max(ax, bx) : x;
                lo = std::min(lo, x); hi = std::max(hi, x2);
            }
        }
        if (lo > hi) continue;
        int x0 = std::max(0, int(std::ceil(lo - 0.5))), x1 = std::min(c.w - 1, int(std::floor(hi - 0.5)));
        if (x0 > x1 && lo >= 0 && lo < c.w) x0 = x1 = int(lo);
        uint32_t* row = c.px + size_t(y) * c.w;
        for (int x = x0; x <= x1; ++x) blend_px(row[x], color, alpha);
    }
}

void line_a(const RgbaCanvas& c, double fx0, double fy0, double fx1, double fy1, uint32_t color, int alpha) {
    int x0 = int(std::floor(fx0)), y0 = int(std::floor(fy0)), x1 = int(std::floor(fx1)), y1 = int(std::floor(fy1));
    if (std::max(x0, x1) < 0 || std::min(x0, x1) >= c.w || std::max(y0, y1) < 0 || std::min(y0, y1) >= c.h) return;
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int n = 0; n < 8000; ++n) {
        if (x0 >= 0 && y0 >= 0 && x0 < c.w && y0 < c.h) blend_px(c.px[size_t(y0) * c.w + x0], color, alpha);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void rect_outline(const RgbaCanvas& c, int x, int y, int w, int h, uint32_t col) {
    c.line(x, y, x + w - 1, y, col);
    c.line(x, y + h - 1, x + w - 1, y + h - 1, col);
    c.line(x, y, x, y + h - 1, col);
    c.line(x + w - 1, y, x + w - 1, y + h - 1, col);
}

// Premultiplied ARGB `src` scaled by f/256, composited over dst.
inline uint32_t over_scaled(uint32_t s, uint32_t d, unsigned f) {
    unsigned a = ((s >> 24) * f) >> 8;
    if (!a) return d;
    unsigned ia = 255 - a;
    unsigned r = ((((s >> 16) & 255) * f) >> 8) + ((((d >> 16) & 255) * ia) / 255);
    unsigned g = ((((s >> 8) & 255) * f) >> 8) + ((((d >> 8) & 255) * ia) / 255);
    unsigned b = (((s & 255) * f) >> 8) + (((d & 255) * ia) / 255);
    return 0xFF000000u | (std::min(r, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(b, 255u);
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::vector<std::string> split_tags(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s + ",") {
        if (ch == ',' || ch == ';') {
            std::string t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    return out;
}

std::string join_tags(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& t : v) s += (s.empty() ? "" : ", ") + t;
    return s;
}

#ifndef __EMSCRIPTEN__
std::string bundle_path() {
    const char* home = std::getenv("HOME");
    std::string base = home ? std::string(home) + "/.pagan" : ".pagan";
    mkdir(base.c_str(), 0755);
    return base + "/editor_bundle.json";
}
#endif

}  // namespace

#if PG_HAVE_DRESSER
namespace {
// What a dressing depends on: the area without its storage version.
std::string dress_key(const EditArea& a) {
    EditArea c = a;
    c.revision = 0;
    return c.to_json();
}
}  // namespace
// The dressed area as a picture: the dresser's items rendered by the real world renderer
// (WorldView) in an empty map, kept with its position in the editor's screen space.
struct EditMode::Dressed {
    std::string area;
    uint32_t seed = 0;
    std::string src;          // the area JSON it was made from
    long sx0 = 0, sy0 = 0;    // native screen point of the picture's top-left
    int w = 0, h = 0;
    std::vector<uint32_t> px; // 0 = transparent
    size_t items = 0;
    bool ok = false;
    std::vector<std::string> report;  // the validation result, POI choices and warnings
    std::string json;                 // dressed_to_json (docs/area_format.md section 4)
};
namespace {
TileKit* g_kit = nullptr;
std::string g_kit_err;
}  // namespace
#else
struct EditMode::Dressed {
    std::string area;
    uint32_t seed = 0;
};
#endif

const std::vector<std::string>& EditMode::themes() {
    static const std::vector<std::string> t = {"cave", "tenebrae", "cemetery", "catacombs", "lava", "plateau", "beach", "fortress", "ether"};
    return t;
}

EditMode::EditMode(const GameData& data) : data_(data), cache_(data) {
    for (unsigned i = 0; i < data.fonts.count(); ++i) fonts_.emplace_back(data.fonts.shape(i), int(i));
    fps_ = compute_footprints(data);
#ifdef __EMSCRIPTEN__
    cache_.set_budget_bytes(160u << 20);
#else
    cache_.set_budget_bytes(512u << 20);
#endif
}

EditMode::~EditMode() = default;

bool EditMode::dresser_available() const {
#if PG_HAVE_DRESSER
    if (g_kit) return true;
    if (kit_path_.empty() || !g_kit_err.empty()) return false;
    auto* k = new TileKit;
    std::string err;
    if (load_tilekit(kit_path_, *k, err)) {
        g_kit = k;
        PG_LOG("editor: tile kit %s", kit_path_.c_str());
        return true;
    }
    delete k;
    g_kit_err = err.empty() ? "no tile kit" : err;
    PG_LOG("editor: no tile kit (%s): Dress is off", g_kit_err.c_str());
    return false;
#else
    return false;
#endif
}

// --- Loading and saving --------------------------------------------------------------------

bool EditMode::load_all() {
    if (storage_ && !storage_->ready()) return false;
    std::string err;
    bool have_layout = false;
    doc = EditDoc{};
    saved_text_.clear();
    saved_rev_.clear();
    saved_layout_.clear();
    layout_rev_ = 0;
    auto take_layout = [&](const StorageResult& r, bool own) {
        if (!r.ok) return;
        WorldLayout wl;
        if (!wl.from_json(r.text, err)) { PG_LOG("editor: layout: %s", err.c_str()); return; }
        doc.layout = std::move(wl);
        if (own) { layout_rev_ = r.version; doc.layout.revision = r.version; }
        else doc.layout.revision = 0;
        have_layout = true;
    };
    if (storage_) storage_->load_layout([&](const StorageResult& r) { take_layout(r, true); });
    bool seeded_layout = false;
    replaced_stale_ = false;
    if (!have_layout && seed_) {
        seed_->load_layout([&](const StorageResult& r) { take_layout(r, false); });
        seeded_layout = have_layout;
    } else if (have_layout && seed_) {
        // A stored layout of an older generation than the shipped one (e.g. a plain atlas layout
        // saved before the stitched world, or the overlapping revision 1): the shipped groups,
        // seams, links and layers replace it; the painted areas and their places are kept. The
        // next save stores the result (the stored version stays in the history).
        auto rev_of = [](const std::string& text) {
            WorldLayoutFile f;
            std::string e;
            return f.from_json(text, e) ? f.layout_revision : 0;
        };
        std::string mine = doc.layout.to_json();
        seed_->load_layout([&](const StorageResult& r) {
            if (!r.ok) return;
            int have = rev_of(mine), ship = rev_of(r.text);
            if (ship <= have) return;
            WorldLayout wl;
            std::string e2;
            if (!wl.from_json(r.text, e2)) return;
            wl.areas = doc.layout.areas;
            wl.revision = doc.layout.revision;
            doc.layout = std::move(wl);
            replaced_stale_ = true;
            PG_LOG("editor: the stored layout (generation %d) is older than the shipped one (%d): replaced, areas kept", have, ship);
        });
    }
    if (!have_layout) {
        doc.layout.atlas = fallback_.groups.empty() ? generate_atlas_layout(data_) : fallback_;
        doc.layout.revision = 0;
    }
    auto load_from = [&](EditorStorage& st, bool own) {
        std::vector<StoredInfo> ids;
        st.list_areas([&](const StorageResult& r) { if (r.ok) ids = r.list; });
        for (const StoredInfo& info : ids)
            st.load_area(info.id, [&](const StorageResult& r) {
                if (!r.ok) return;
                EditArea a;
                if (!a.from_json(r.text, err)) { PG_LOG("editor: area %s: %s", info.id.c_str(), err.c_str()); return; }
                a.id = info.id;
                a.revision = own ? r.version : 0;
                if (doc.find(a.id) < 0) doc.areas.push_back(std::move(a));
            });
    };
    if (storage_) load_from(*storage_, true);
    if (doc.areas.empty() && seed_ && (seeded_layout || !have_layout)) load_from(*seed_, false);
    for (EditArea& a : doc.areas)
        for (const LayoutArea& la : doc.layout.areas)
            if (la.id == a.id) { std::copy(la.offset, la.offset + 3, a.origin); a.layer = la.layer; }
    for (EditArea& a : doc.areas) {
        a.normalize();
        saved_rev_[a.id] = a.revision;
        // Seeded areas are not in this storage yet: unchanged ones need no save.
        saved_text_[a.id] = a.to_json();
    }
    doc.sync_layout();
    saved_layout_ = doc.layout.to_json();  // (a layout from the atlas or the seed is saved with the first change)
    if (replaced_stale_) {
        saved_layout_.clear();  // unsaved: Save stores the new layout
        say("The world layout was updated to the new layered one (your areas are kept). Save to store it.", 400);
    }
    doc.clear_history();
    last_changes_ = doc.changes;
    refresh_exclusions();
    loaded_ = true;
    ++save_gen_;
    publish_state();
    PG_LOG("editor: %zu areas, %zu map groups, layout %s (storage %s)", doc.areas.size(), doc.layout.atlas.groups.size(),
           have_layout ? (seeded_layout ? "from the shipped data" : "loaded") : "from the atlas", storage_ ? storage_->name().c_str() : "none");
    return true;
}

bool EditMode::dirty() const {
    if (dirty_stamp_ == doc.changes && dirty_gen_ == save_gen_ && drag_ == Drag::kNone) return dirty_cache_;
    dirty_cache_ = compute_dirty();
    dirty_stamp_ = doc.changes;
    dirty_gen_ = save_gen_;
    return dirty_cache_;
}

bool EditMode::compute_dirty() const {
    for (const EditArea& a : doc.areas) {
        auto it = saved_text_.find(a.id);
        if (it == saved_text_.end() || it->second != a.to_json()) return true;
    }
    for (const auto& [id, t] : saved_text_)
        if (doc.find(id) < 0) return true;
    WorldLayout l = doc.layout;
    l.areas.clear();
    for (const EditArea& a0 : doc.areas) {
        EditArea a = a0;
        a.normalize();
        LayoutArea la;
        la.id = a.id;
        la.name = a.name;
        std::copy(a.origin, a.origin + 3, la.offset);
        la.layer = a.layer;
        l.areas.push_back(la);
    }
    return l.to_json() != saved_layout_;
}

void EditMode::save(bool force) {
    if (!storage_) { say("No storage"); return; }
    if (doc.in_change()) doc.commit();
    force = force || force_next_;
    force_next_ = false;
    doc.sync_layout();
    int saved = 0, conflicts = 0, errors = 0;
    std::string last_err;
    for (const EditArea& a : doc.areas) {
        std::string text = a.to_json();
        auto it = saved_text_.find(a.id);
        if (it != saved_text_.end() && it->second == text) continue;
        int base = force ? -1 : (saved_rev_.count(a.id) ? saved_rev_[a.id] : 0);
        std::string id = a.id;
        storage_->save_area(id, text, base, [this, id, &saved, &conflicts, &errors, &last_err](const StorageResult& r) {
            int i = doc.find(id);
            if (r.ok && i >= 0) {
                doc.areas[size_t(i)].revision = r.version;
                saved_rev_[id] = r.version;
                saved_text_[id] = doc.areas[size_t(i)].to_json();
                ++saved;
            } else if (r.conflict) {
                ++conflicts;
                last_err = id + ": " + r.err;
            } else {
                ++errors;
                last_err = id + ": " + r.err;
            }
        });
    }
    std::vector<std::string> gone;
    for (const auto& [id, t] : saved_text_)
        if (doc.find(id) < 0) gone.push_back(id);
    for (const std::string& id : gone) {
        int base = force ? -1 : saved_rev_[id];
        storage_->delete_area(id, base, [this, id, &saved, &conflicts, &errors, &last_err](const StorageResult& r) {
            if (r.ok || (!r.conflict && r.err.rfind("no area", 0) == 0)) {
                saved_text_.erase(id);
                saved_rev_.erase(id);
                ++saved;
            } else if (r.conflict) {
                ++conflicts;
                last_err = id + ": " + r.err;
            } else {
                ++errors;
                last_err = id + ": " + r.err;
            }
        });
    }
    doc.layout.revision = layout_rev_;
    std::string lt = doc.layout.to_json();
    if (lt != saved_layout_) {
        storage_->save_layout(lt, force ? -1 : layout_rev_, [this, &saved, &conflicts, &errors, &last_err](const StorageResult& r) {
            if (r.ok) {
                layout_rev_ = r.version;
                doc.layout.revision = r.version;
                saved_layout_ = doc.layout.to_json();
                ++saved;
            } else if (r.conflict) {
                ++conflicts;
                last_err = "layout: " + r.err;
            } else {
                ++errors;
                last_err = "layout: " + r.err;
            }
        });
    }
    if (conflicts) {
        force_next_ = true;
        say("Saved elsewhere meanwhile (" + last_err + "). Save again to overwrite.", 300);
    } else if (errors) {
        say("Save failed: " + last_err, 300);
    } else if (saved) {
        say("Saved (" + std::to_string(saved) + " change" + (saved == 1 ? "" : "s") + ") to " + storage_->name());
    } else {
        say("Nothing to save");
    }
    ++save_gen_;
    publish_state();
    PG_LOG("editor: save: %d written, %d conflicts, %d errors %s", saved, conflicts, errors, last_err.c_str());
    // A network backend uploads now (editor/server_storage.h); force overwrites its conflicts.
    storage_->after_save(!autosaving_, force);
}

void EditMode::autosave_tick() {
    if (doc.changes != last_changes_) { last_changes_ = doc.changes; changed_ticks_ = 0; return; }
    ++changed_ticks_;
    if (!autosave_ || changed_ticks_ != 75 || drag_ != Drag::kNone || doc.in_change() || dialog_) return;
    if (!dirty()) return;
    std::string keep = message;
    int kt = message_ticks;
    autosaving_ = true;
    save(false);
    autosaving_ = false;
    if (message.rfind("Saved (", 0) == 0) {
        message = "Autosaved";
        message_ticks = 60;
    } else if (message == "Nothing to save") {
        message = keep;
        message_ticks = kt;
    }
}

void EditMode::export_bundle() {
    std::string b = doc.bundle_json();
#ifdef __EMSCRIPTEN__
    EM_ASM({
        try {
            const s = UTF8ToString($0);
            const a = document.createElement('a');
            a.href = URL.createObjectURL(new Blob([s], {type: 'application/json'}));
            a.download = 'pagan-editor.json';
            document.body.appendChild(a);
            a.click();
            a.remove();
            Module.pgEditDownloads = (Module.pgEditDownloads || 0) + 1;
        } catch (e) { console.warn('editor: download failed', e); }
    }, b.c_str());
    say("Downloaded pagan-editor.json (" + std::to_string(doc.areas.size()) + " areas + the layout)");
#else
    std::string p = bundle_path();
    std::ofstream f(p, std::ios::binary);
    f << b;
    say(f ? "Wrote " + p : "Could not write " + p);
#endif
}

void EditMode::import_bundle() {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        try {
            const i = document.createElement('input');
            i.type = 'file';
            i.accept = '.json,application/json';
            i.onchange = () => {
                const f = i.files && i.files[0];
                if (!f) return;
                f.text().then(t => { Module.pgEditUpload = t; }, e => console.warn('editor: upload failed', e));
            };
            i.click();
        } catch (e) { console.warn('editor: upload failed', e); }
    });
    say("Choose a pagan-editor.json (or an area .json) to load");
#else
    std::string p = bundle_path(), text;
    std::ifstream f(p, std::ios::binary);
    if (!f) { say("No " + p + " to load"); return; }
    std::stringstream ss;
    ss << f.rdbuf();
    import_text(ss.str());
#endif
}

bool EditMode::import_text(const std::string& text) {
    std::string err;
    if (!doc.load_bundle(text, err)) { say("Could not load: " + err, 240); return false; }
    // The storage's versions stay the base for the next save (an import is a local edit).
    for (EditArea& a : doc.areas) a.revision = saved_rev_.count(a.id) ? saved_rev_[a.id] : 0;
    doc.layout.revision = layout_rev_;
    refresh_exclusions();
    if (!doc.areas.empty()) select_area(doc.areas.back().id);
    say("Loaded " + std::to_string(doc.areas.size()) + " areas (Save keeps them)");
    return true;
}

void EditMode::dress() {
    EditArea* a = sel();
    if (!a) { say("Select an area to dress"); return; }
    if (!dresser_available()) { say("The map dresser is not built in yet"); return; }
#if PG_HAVE_DRESSER
    auto d = std::make_unique<Dressed>();
    d->area = a->id;
    d->seed = a->seed;
    d->src = dress_key(*a);
    PaintedArea pa;
    std::string err;
    if (!parse_area(d->src, pa, err)) { say("Dress: " + err); return; }
    DressResult r = pg::dress(pa, *g_kit, a->seed);
    std::vector<ItemRecord> recs = to_records(r, false);  // no roofs: the inside stays visible
    d->items = recs.size();
    d->ok = r.validation.ok;
    d->json = dressed_to_json(pa, r);
    {
        const Validation& v = r.validation;
        auto yn = [](bool b) { return b ? "yes" : "NO"; };
        char line[256];
        std::snprintf(line, sizeof line, "Dresser checks: %s  (%zu items, seed %u)", v.ok ? "passed" : "PROBLEMS", recs.size(), r.seed);
        d->report.push_back(line);
        std::snprintf(line, sizeof line, "floor covered %s (%d of %d cells uncovered), walls closed %s (%d gaps), walkable %s (%d of %d spots)",
                      yn(v.floor_covered), v.uncovered_cells, v.floor_cells, yn(v.walls_closed), v.wall_gaps, yn(v.walkable),
                      v.reachable, v.positions);
        d->report.push_back(line);
        std::snprintf(line, sizeof line, "overlaps %d, unsupported %d, entrances %zu", v.overlaps, v.unsupported, r.entrances.size());
        d->report.push_back(line);
        if (!v.unreachable_pois.empty()) {
            std::string u = "unreachable markers:";
            for (const std::string& id : v.unreachable_pois) u += " " + id;
            d->report.push_back(u);
        }
        for (const PoiResult& p : r.pois) {
            std::snprintf(line, sizeof line, "marker %s: %s%s%s", p.id.c_str(), p.placed ? "" : "NOT PLACED ",
                          p.prefab.empty() ? "no prefab" : p.prefab.c_str(), p.label.empty() ? "" : (" (" + p.label + ")").c_str());
            d->report.push_back(line);
        }
        for (const std::string& w : r.warnings) d->report.push_back("warning: " + w);
        for (const std::string& pr : v.problems) d->report.push_back("problem: " + pr);
        for (const std::string& l : d->report) PG_LOG("editor: dress %s: %s", a->id.c_str(), l.c_str());
    }
    // Render the items in an empty map at (4096, 4096) and frame them.
    const int P = 4096;
    CurrentMap map(data_);
    map.reset(255);
    std::vector<uint16_t> ids = place_items(map, recs, P, P, 0);
    long x0 = LONG_MAX, y0 = LONG_MAX, x1 = LONG_MIN, y1 = LONG_MIN;
    for (uint16_t id : ids) {
        const Item& it = map.item(id);
        const ShapeFrame* f = data_.shapes.frame(it.shape, it.frame);
        if (!f) continue;
        long sx = it.x / 4 - it.y / 4, sy = it.x / 8 + it.y / 8 - it.z;
        x0 = std::min(x0, sx - std::max<long>(f->xoff, f->w - f->xoff));
        x1 = std::max(x1, sx + std::max<long>(f->xoff, f->w - f->xoff));
        y0 = std::min(y0, sy - f->yoff);
        y1 = std::max(y1, sy - f->yoff + f->h);
    }
    if (x0 > x1) { say("Dress: nothing to place"); return; }
    int W = int(std::min(4096L, x1 - x0 + 16)) & ~1, H = int(std::min(4096L, y1 - y0 + 16)) & ~1;
    Surface8 surf(W, H);
    surf.clear(0);
    WorldView view(data_);
    WorldView::Options vo;
    vo.roof_hiding = false;
    vo.use_screen_camera = true;
    vo.camera_sx = int((x0 + x1) / 2);
    vo.camera_sy = int((y0 + y1) / 2);
    int cam[3] = {2 * vo.camera_sx + 4 * vo.camera_sy, 4 * vo.camera_sy - 2 * vo.camera_sx, 0};
    view.render(map, surf, cam, cam, vo);
    d->w = W;
    d->h = H;
    d->px.assign(size_t(W) * H, 0);
    for (size_t i = 0; i < d->px.size(); ++i) {
        uint8_t ix = surf.data()[i];
        if (!ix) continue;
        const auto& c = data_.palette.rgb8[ix];
        d->px[i] = 0xFF000000u | (uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | c[2];
    }
    // The picture's top-left in the editor's (flattened) screen space: placed point + origin - P.
    long ox = pa.origin[0] - P, oy = pa.origin[1] - P;
    d->sx0 = vo.camera_sx - W / 2 + (ox - oy) / 4;
    d->sy0 = vo.camera_sy - H / 2 + (ox + oy) / 8;
    say("Dressed " + a->name + ": " + std::to_string(d->items) + " items, seed " + std::to_string(a->seed) +
        (d->ok ? ", checks passed" : ", the dresser found problems (see the report)") + ". Undress: back to the paint", 240);
    dressed_ = std::move(d);
    publish_state();
#endif
}

size_t EditMode::dress_items_() const {
#if PG_HAVE_DRESSER
    if (dressed_) return dressed_->items;
#endif
    return 0;
}

bool EditMode::dress_ok_() const {
#if PG_HAVE_DRESSER
    if (dressed_) return dressed_->ok;
#endif
    return false;
}

std::vector<std::string> EditMode::dress_report() const {
#if PG_HAVE_DRESSER
    if (dressed_) return dressed_->report;
#endif
    return {};
}

std::string EditMode::dressed_json() const {
#if PG_HAVE_DRESSER
    if (dressed_) return dressed_->json;
#endif
    return {};
}

void EditMode::draw_dress_report(RgbaCanvas& c) {
#if PG_HAVE_DRESSER
    if (!dressed_ || dialog_ || help || dressed_->report.empty()) return;
    const ShapeFont& f = fonts_.size() > 6 ? fonts_[6] : fonts_[0];
    const int u = ui_, lh = (f.line_advance() + 3) * u;
    const auto& rep = dressed_->report;
    size_t n = std::min<size_t>(rep.size(), 9);
    int maxw = (panel ? panel_x_ : W_) - 12 * u, w = 0;
    for (size_t i = 0; i < n; ++i) w = std::max(w, (f.text_width(rep[i]) + 10) * u);
    w = std::min(w, maxw);
    int h = int(n) * lh + 6 * u + (rep.size() > n ? lh : 0);
    int x = 4 * u, y = bottom_y_ - (f.line_advance() + 8) * u - h;
    c.shade(x, y, w, h, 0xFF000000u, 190);
    rect_outline(c, x, y, w, h, dressed_->ok ? 0xFF60C060u : 0xFFE06040u);
    for (size_t i = 0; i < n; ++i) {
        std::string l = rep[i];
        while (!l.empty() && (f.text_width(l) + 10) * u > w) l.pop_back();
        c.text(f, x + 5 * u, y + 3 * u + int(i) * lh, l, u);
    }
    if (rep.size() > n) c.text(f, x + 5 * u, y + 3 * u + int(n) * lh, "... " + std::to_string(rep.size() - n) + " more in the log", u);
#else
    (void)c;
#endif
}

void EditMode::draw_dressed(RgbaCanvas& c) {
#if PG_HAVE_DRESSER
    if (!dressed_) return;
    int ai = doc.find(dressed_->area);
    if (ai < 0) { dressed_.reset(); return; }
    const EditArea& a = doc.areas[size_t(ai)];
    if (a.layer != layer) return;
    if (dressed_->seed != a.seed || dressed_->src != dress_key(a)) {
        if (drag_ != Drag::kNone) return;  // re-dressed when the stroke ends
        std::string keep = selected;
        selected = a.id;
        dress();
        selected = keep;
        if (!dressed_) return;
    }
    const Dressed& d = *dressed_;
    const double s = std::ldexp(1.0, level_), k = 1.0 / s;
    int u0 = std::max(0, int(std::floor((d.sx0 - cx_) * k + W_ / 2.0)));
    int v0 = std::max(top_h_, int(std::floor((d.sy0 - cy_) * k + H_ / 2.0)));
    int u1 = std::min(W_, int(std::ceil((d.sx0 + d.w - cx_) * k + W_ / 2.0)));
    int v1 = std::min(bottom_y_, int(std::ceil((d.sy0 + d.h - cy_) * k + H_ / 2.0)));
    for (int v = v0; v < v1; ++v) {
        long py = long(std::floor((v - H_ / 2.0) * s + cy_)) - d.sy0;
        if (py < 0 || py >= d.h) continue;
        const uint32_t* row = &d.px[size_t(py) * d.w];
        uint32_t* out = c.px + size_t(v) * W_;
        for (int u = u0; u < u1; ++u) {
            long px = long(std::floor((u - W_ / 2.0) * s + cx_)) - d.sx0;
            if (px < 0 || px >= d.w) continue;
            if (uint32_t p = row[px]) out[u] = p;
        }
    }
#else
    (void)c;
#endif
}

void EditMode::reroll() {
    EditArea* a = sel();
    if (!a) { say("Select an area first"); return; }
    uint32_t s = a->seed * 1103515245u + 12345u;
    s = (s >> 8) % 100000u + 1;
    doc.set_area_seed(a->id, s);  // undoable: Undo brings the previous variation back
    if (dressed_) dress();
    else say("Seed " + std::to_string(s));
}

// --- View ----------------------------------------------------------------------------------

void EditMode::world_to_canvas(double x, double y, double z, double& u, double& v) const {
    double k = std::ldexp(1.0, -level_);
    u = ((x - y) / 4.0 - cx_) * k + W_ / 2.0;
    v = ((x + y) / 8.0 - z - cy_) * k + H_ / 2.0;
}

void EditMode::canvas_to_world(double u, double v, double z, double& x, double& y) const {
    double s = std::ldexp(1.0, level_);
    double nx = (u - W_ / 2.0) * s + cx_, ny = (v - H_ / 2.0) * s + cy_;
    x = 2 * nx + 4 * (ny + z);
    y = 4 * (ny + z) - 2 * nx;
}

void EditMode::set_level(int lv, double u, double v) {
    lv = std::clamp(lv, -2, TileCache::kMaxLevel);
    double s0 = std::ldexp(1.0, level_), s1 = std::ldexp(1.0, lv);
    double nx = (u - W_ / 2.0) * s0 + cx_, ny = (v - H_ / 2.0) * s0 + cy_;
    level_ = lv;
    cx_ = nx - (u - W_ / 2.0) * s1;
    cy_ = ny - (v - H_ / 2.0) * s1;
}

void EditMode::show_world_point(int x, int y, int level) {
    level_ = std::clamp(level, -2, TileCache::kMaxLevel);
    cx_ = (x - y) / 4.0;
    cy_ = (x + y) / 8.0;
}

void EditMode::show_map_point(unsigned map, int x, int y, int z, int level) {
    int gi = doc.layout.atlas.group_of(map);
    if (gi < 0) { show_world_point(16384, 16384, level); return; }
    const AtlasGroup& g = doc.layout.atlas.groups[size_t(gi)];
    layer = g.layer;
    show_world_point(x + g.dx, y + g.dy, level);
    cy_ -= z;
}

std::vector<PlacedMap> EditMode::placed(int lay) const {
    std::vector<PlacedMap> out, top;
    for (size_t gi = 0; gi < doc.layout.atlas.groups.size(); ++gi) {
        const AtlasGroup& g = doc.layout.atlas.groups[gi];
        int dx, dy, gl;
        shown_place(int(gi), dx, dy, gl);
        if (gl != lay) continue;
        int sx, sy;
        atlas_screen_offset(dx, dy, sx, sy);
        // The selected (and the grabbed) segment is drawn last: on top where segments overlap.
        bool up = int(gi) == selected_group || multi.count(int(gi)) ||
                  std::find(drag_set_.begin(), drag_set_.end(), int(gi)) != drag_set_.end();
        for (unsigned m : g.maps) (up ? top : out).push_back({m, sx, sy});
    }
    out.insert(out.end(), top.begin(), top.end());
    return out;
}

void EditMode::refresh_exclusions() {
    // The editor draws every segment whole: all the FIXED and NONFIXED items of its maps, with no
    // seam de-duplication and no "densest map per chunk" choice. Where segments overlap both are
    // drawn (the selected one on top) and the outlines mark the overlap.
    for (const AtlasGroup& g : doc.layout.atlas.groups)
        for (unsigned m : g.maps) cache_.set_exclusion(m, std::bitset<64 * 64>());
    overlap_stamp_ = -1;
}

void EditMode::draw_maps(std::vector<uint32_t>& out) {
    double deadline = budget_ms_ < 0 ? std::numeric_limits<double>::infinity() : TileCache::now_ms() + budget_ms_;
    std::fill(out.begin(), out.end(), kBackground);
    std::vector<uint32_t> buf(out.size());
    int missing = 0;
    auto compose = [&](int lay, unsigned f) {
        if (hidden_layers.count(lay)) return;
        std::vector<PlacedMap> pm = placed(lay);
        if (pm.empty()) return;
        if (level_ >= 0) {
            missing += draw_tiles(cache_, pm, level_, int(std::lround(cx_)), int(std::lround(cy_)), buf.data(), W_, H_, deadline, 0);
            for (size_t i = 0; i < out.size(); ++i) out[i] = over_scaled(buf[i], out[i], f);
        } else {
            int m = 1 << -level_, ww = W_ / m, hh = H_ / m;
            std::vector<uint32_t> small(size_t(ww) * hh);
            missing += draw_tiles(cache_, pm, 0, int(std::lround(cx_)), int(std::lround(cy_)), small.data(), ww, hh, deadline, 0);
            for (int v = 0; v < H_; ++v) {
                const uint32_t* src = &small[size_t(std::min(hh - 1, v / m)) * ww];
                uint32_t* dst = &out[size_t(v) * W_];
                for (int u = 0; u < W_; ++u) dst[u] = over_scaled(src[std::min(ww - 1, u / m)], dst[u], f);
            }
        }
    };
    compose(layer - 1, 80);
    compose(layer, 256);
    compose(layer + 1, 56);
    pending_ = size_t(missing);
}

// --- Areas on screen -------------------------------------------------------------------------

void EditMode::draw_areas(RgbaCanvas& c, uint32_t*) {
    const double k = std::ldexp(1.0, -level_);
    // The layer below first, then this layer, then the one above (see-through).
    std::vector<const EditArea*> order;
    for (int want : {-1, 0, 1})
        for (const EditArea& a : doc.areas)
            if (a.layer - layer == want && !hidden_layers.count(a.layer)) order.push_back(&a);
    for (const EditArea* ap : order) {
        const EditArea& a = *ap;
        int d = a.layer - layer;
        const bool here = d == 0;
        const bool dressed = here && dressed_ && dressed_->area == a.id;
        const int alpha = here ? 215 : (d < 0 ? 165 : 80);
        struct Q { int cx, cy; const AreaCell* c; };
        std::vector<Q> cells;
        for (const auto& [key, ce] : a.cells) cells.push_back({key.second, key.first, &ce});
        std::sort(cells.begin(), cells.end(), [](const Q& p, const Q& q) {
            if (p.cx + p.cy != q.cx + q.cy) return p.cx + p.cy < q.cx + q.cy;
            return p.c->height < q.c->height;
        });
        const double C = a.cell;
        for (const Q& q : cells) {
            if (dressed) break;  // drawn as the dresser's items (draw_dressed)
            double x0 = a.origin[0] + q.cx * C, y0 = a.origin[1] + q.cy * C, x1 = x0 + C, y1 = y0 + C;
            double z = a.base_z + q.c->height * a.level_z;
            uint32_t col = kind_color(q.c->kind);
            if (!q.c->theme.empty()) col = darker(col, 88);
            double top = z + (q.c->kind == cell::kWall && here ? 32 : 0);
            double us[4], vs[4];
            world_to_canvas(x0, y0, top, us[0], vs[0]);
            world_to_canvas(x1, y0, top, us[1], vs[1]);
            world_to_canvas(x1, y1, top, us[2], vs[2]);
            world_to_canvas(x0, y1, top, us[3], vs[3]);
            if (us[1] < -8 || us[3] > W_ + 8 || vs[0] > H_ + 64 * k + 8 || vs[2] < -8) continue;
            if (here && top > 0) {
                // Sides down to z 0 (the area's base): left face W-S, right face S-E.
                double drop = top * k;
                double lx[4] = {us[3], us[2], us[2], us[3]}, ly[4] = {vs[3], vs[2], vs[2] + drop, vs[3] + drop};
                double rx[4] = {us[2], us[1], us[1], us[2]}, ry[4] = {vs[2], vs[1], vs[1] + drop, vs[2] + drop};
                fill_poly(c, lx, ly, 4, darker(col, 62), alpha);
                fill_poly(c, rx, ry, 4, darker(col, 45), alpha);
            }
            fill_poly(c, us, vs, 4, col, alpha);
            if (C * k / 2 >= 10) {
                uint32_t e = darker(col, 55);
                for (int i = 0; i < 4; ++i) line_a(c, us[i], vs[i], us[(i + 1) % 4], vs[(i + 1) % 4], e, here ? 200 : 80);
            }
            if (here && q.c->kind == cell::kDoor && C * k / 2 >= 8) {
                double cu = (us[0] + us[2]) / 2, cv = (vs[0] + vs[2]) / 2;
                c.shade(int(cu) - 1, int(cv) - 3, 3, 6, 0xFF3A2010u, 255);
            }
            if (here && q.c->height && C * k / 2 >= 26) {
                const ShapeFont& f = fonts_.size() > 6 ? fonts_[6] : fonts_[0];
                std::string hs(1, height_digit(q.c->height));
                c.text(f, int((us[0] + us[2]) / 2) - 2 * ui_, int((vs[0] + vs[2]) / 2) - 4 * ui_, hs, ui_);
            }
        }
        // Outline, label, POIs.
        int bx0, by0, bx1, by1;
        if (!a.bounds(bx0, by0, bx1, by1)) {
            bx0 = by0 = 0;
            bx1 = by1 = 0;
        }
        const bool is_sel = a.id == selected;
        if (!here) {
            double x0 = a.origin[0] + bx0 * C, y0 = a.origin[1] + by0 * C, x1 = a.origin[0] + (bx1 + 1) * C, y1 = a.origin[1] + (by1 + 1) * C;
            double us[4], vs[4];
            world_to_canvas(x0, y0, a.base_z, us[0], vs[0]);
            world_to_canvas(x1, y0, a.base_z, us[1], vs[1]);
            world_to_canvas(x1, y1, a.base_z, us[2], vs[2]);
            world_to_canvas(x0, y1, a.base_z, us[3], vs[3]);
            uint32_t oc = d < 0 ? 0xFF60A0FFu : 0xFFA0FFE0u;
            for (int i = 0; i < 4; ++i) {
                line_a(c, us[i], vs[i], us[(i + 1) % 4], vs[(i + 1) % 4], oc, 230);
                line_a(c, us[i], vs[i] + 1, us[(i + 1) % 4], vs[(i + 1) % 4] + 1, oc, 140);
            }
            const ShapeFont& f = fonts_.size() > 6 ? fonts_[6] : fonts_[0];
            std::string lab = (a.name.empty() ? a.id : a.name) + (d < 0 ? " (layer below)" : " (layer above)");
            c.text(f, int(us[2]) - (f.text_width(lab) / 2) * ui_, int(vs[2]) + 2 * ui_, lab, ui_);
        }
        if (here) {
            double x0 = a.origin[0] + bx0 * C, y0 = a.origin[1] + by0 * C, x1 = a.origin[0] + (bx1 + 1) * C, y1 = a.origin[1] + (by1 + 1) * C;
            double us[4], vs[4];
            world_to_canvas(x0, y0, a.base_z, us[0], vs[0]);
            world_to_canvas(x1, y0, a.base_z, us[1], vs[1]);
            world_to_canvas(x1, y1, a.base_z, us[2], vs[2]);
            world_to_canvas(x0, y1, a.base_z, us[3], vs[3]);
            uint32_t oc = is_sel ? 0xFFFFE060u : 0xFF8890A0u;
            for (int i = 0; i < 4; ++i) line_a(c, us[i], vs[i], us[(i + 1) % 4], vs[(i + 1) % 4], oc, is_sel ? 255 : 150);
            const ShapeFont& f = fonts_.size() > 6 ? fonts_[6] : fonts_[0];
            std::string lab = (a.name.empty() ? a.id : a.name) + " (" + a.theme + ")";
            int tw = (f.text_width(lab) + 6) * ui_;
            int lu = int(us[0]) - tw / 2, lv = int(vs[0]) - (f.line_advance() + 6) * ui_;
            c.shade(lu, lv, tw, (f.line_advance() + 3) * ui_, 0xFF000000u, is_sel ? 200 : 130);
            c.text(f, lu + 3 * ui_, lv + ui_, lab, ui_);
            for (const AreaPoi& p : a.pois) {
                const AreaCell* pc = a.at(p.x, p.y);
                double z = a.base_z + (pc ? pc->height * a.level_z : 0);
                double pu, pv;
                world_to_canvas(a.origin[0] + (p.x + 0.5) * C, a.origin[1] + (p.y + 0.5) * C, z, pu, pv);
                if (pu < -100 || pu > W_ + 100 || pv < -40 || pv > H_ + 40) continue;
                int r = 3 * ui_ + 1, stem = 9 * ui_;
                line_a(c, pu, pv, pu, pv - stem, 0xFF202020u, 255);
                line_a(c, pu + 1, pv, pu + 1, pv - stem, 0xFF202020u, 255);
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) {
                        int dd = dx * dx + dy * dy;
                        if (dd > r * r) continue;
                        int x = int(pu) + dx, y = int(pv) - stem + dy;
                        if (x < 0 || y < 0 || x >= W_ || y >= H_) continue;
                        c.px[size_t(y) * W_ + x] = dd > (r - 1) * (r - 1) ? 0xFFFFE060u : 0xFFD02828u;
                    }
                if (level_ <= 4) {
                    std::string note = p.note.empty() ? p.id : p.note;
                    if (note.size() > 28) note = note.substr(0, 26) + "..";
                    if (!p.tags.empty()) note += " [" + join_tags(p.tags) + "]";
                    int w2 = (f.text_width(note) + 6) * ui_;
                    int tu = int(pu) + r + 2, tv = int(pv) - stem - (f.line_advance() / 2 + 2) * ui_;
                    c.shade(tu, tv, w2, (f.line_advance() + 3) * ui_, 0xFF000000u, 170);
                    c.text(f, tu + 3 * ui_, tv + ui_, note, ui_);
                }
            }
        }
    }
}

void EditMode::draw_grid(RgbaCanvas& c) {
    if (!show_grid) return;
    const double k = std::ldexp(1.0, -level_);
    const EditArea* a = nullptr;
    for (const EditArea& e : doc.areas)
        if (e.id == selected) a = &e;
    const int C = a ? a->cell : 128;
    if (C * k / 2 < 6) return;
    const double ox = a ? a->origin[0] : 0, oy = a ? a->origin[1] : 0;
    const double z = (a ? a->base_z : 0) + height * (a ? a->level_z : 8);
    double xs[4], ys[4];
    canvas_to_world(0, 0, z, xs[0], ys[0]);
    canvas_to_world(W_, 0, z, xs[1], ys[1]);
    canvas_to_world(W_, H_, z, xs[2], ys[2]);
    canvas_to_world(0, H_, z, xs[3], ys[3]);
    double x0 = *std::min_element(xs, xs + 4), x1 = *std::max_element(xs, xs + 4);
    double y0 = *std::min_element(ys, ys + 4), y1 = *std::max_element(ys, ys + 4);
    long i0 = long(std::floor((x0 - ox) / C)), i1 = long(std::ceil((x1 - ox) / C));
    long j0 = long(std::floor((y0 - oy) / C)), j1 = long(std::ceil((y1 - oy) / C));
    // Only around the selected area (its bounds + 6 cells) or the pointer (8 cells).
    int bx0, by0, bx1, by1;
    if (a && a->bounds(bx0, by0, bx1, by1)) {
        i0 = std::max(i0, long(bx0 - 6)); i1 = std::min(i1, long(bx1 + 7));
        j0 = std::max(j0, long(by0 - 6)); j1 = std::min(j1, long(by1 + 7));
    } else {
        double mx, my;
        canvas_to_world(mouse_u_, mouse_v_, z, mx, my);
        long mi = long(std::floor((mx - ox) / C)), mj = long(std::floor((my - oy) / C));
        i0 = std::max(i0, mi - 8); i1 = std::min(i1, mi + 9);
        j0 = std::max(j0, mj - 8); j1 = std::min(j1, mj + 9);
    }
    if (i1 < i0 || j1 < j0 || i1 - i0 > 400 || j1 - j0 > 400) return;
    const int alpha = C * k / 2 >= 20 ? 70 : 40;
    for (long i = i0; i <= i1; ++i) {
        double u0, v0, u1, v1;
        world_to_canvas(ox + i * C, oy + j0 * C, z, u0, v0);
        world_to_canvas(ox + i * C, oy + j1 * C, z, u1, v1);
        line_a(c, u0, v0, u1, v1, 0xFFB0C0D0u, alpha);
    }
    for (long j = j0; j <= j1; ++j) {
        double u0, v0, u1, v1;
        world_to_canvas(ox + i0 * C, oy + j * C, z, u0, v0);
        world_to_canvas(ox + i1 * C, oy + j * C, z, u1, v1);
        line_a(c, u0, v0, u1, v1, 0xFFB0C0D0u, alpha);
    }
}

bool EditMode::cell_under(const EditArea& a, double u, double v, int& cx, int& cy) const {
    // Try the paint height first, then the cell's own height (so raised cells can be hit).
    double x, y;
    canvas_to_world(u, v, a.base_z + height * a.level_z, x, y);
    a.world_cell(int(std::floor(x)), int(std::floor(y)), cx, cy);
    return true;
}

int EditMode::area_under(double u, double v) const {
    for (int i = int(doc.areas.size()) - 1; i >= 0; --i) {
        const EditArea& a = doc.areas[size_t(i)];
        if (a.layer != layer) continue;
        for (int hz : {height, 0}) {
            double x, y;
            canvas_to_world(u, v, a.base_z + hz * a.level_z, x, y);
            int cx, cy;
            a.world_cell(int(std::floor(x)), int(std::floor(y)), cx, cy);
            if (a.at(cx, cy) || a.poi_index(cx, cy) >= 0) return i;
        }
    }
    return -1;
}

int EditMode::group_under(double u, double v) const {
    for (int gi = int(doc.layout.atlas.groups.size()) - 1; gi >= 0; --gi) {
        const AtlasGroup& g = doc.layout.atlas.groups[size_t(gi)];
        if (g.layer != layer) continue;
        for (unsigned m : g.maps)
            for (const MapFootprint& f : fps_) {
                if (f.map != m) continue;
                for (int z : {f.median_z, 0, 48, 96}) {
                    double x, y;
                    canvas_to_world(u, v, z, x, y);
                    long lx = long(std::floor(x)) - g.dx, ly = long(std::floor(y)) - g.dy;
                    if (lx < 0 || ly < 0 || lx >= 32768 || ly >= 32768) continue;
                    if (f.has(int(lx / 512), int(ly / 512))) return gi;
                }
            }
    }
    return -1;
}

EditArea* EditMode::sel() {
    int i = doc.find(selected);
    return i >= 0 ? &doc.areas[size_t(i)] : nullptr;
}

void EditMode::select_area(const std::string& id) {
    selected = id;
    selected_group = -1;
    if (EditArea* a = sel()) {
        layer = a->layer;
        theme = a->theme;
    }
}

EditArea* EditMode::target_area(double u, double v, bool create, int* cx, int* cy) {
    EditArea* a = sel();
    if (a && a->layer != layer) a = nullptr;
    if (!a) {
        int hit = area_under(u, v);
        if (hit >= 0) {
            select_area(doc.areas[size_t(hit)].id);
            a = sel();
        }
    }
    if (!a && create) {
        // A new area with its origin on the global 128 grid under the pointer.
        double x, y;
        canvas_to_world(u, v, height * 8, x, y);
        int org[3] = {int(std::floor(x / 128)) * 128, int(std::floor(y / 128)) * 128, 0};
        std::string name = "Area " + std::to_string(doc.areas.size() + 1);
        if (doc.in_change()) doc.commit();
        EditArea& na = doc.create_area(name, org, layer, theme);
        select_area(na.id);
        a = sel();
    }
    if (a && cx && cy) cell_under(*a, u, v, *cx, *cy);
    return a;
}

PaintValue EditMode::value(bool erase) const {
    PaintValue v;
    if (erase) { v.kind = cell::kVoid; return v; }
    if (kind == kHeightOnly) { v.kind = 0; v.height = height; return v; }
    v.kind = kind;
    v.height = height;
    v.theme = theme;
    return v;
}

void EditMode::stroke_to(int cx, int cy) {
    EditArea* a = sel();
    if (!a) return;
    bool erase = tool == Tool::kErase;
    auto cells = line_cells(brush, brush_size, stroke_cx_, stroke_cy_, cx, cy);
    paint_cells(*a, cells, value(erase));
    stroke_cx_ = cx;
    stroke_cy_ = cy;
}

void EditMode::marker_at(double u, double v) {
    int cx = 0, cy = 0;
    EditArea* a = target_area(u, v, true, &cx, &cy);
    if (!a) return;
    std::string id = a->id;
    int pi = a->poi_index(cx, cy);
    Dialog d;
    d.labels = {"Note", "Tags (comma separated)"};
    if (pi >= 0) {
        d.title = "Point of interest " + a->pois[size_t(pi)].id;
        d.values = {a->pois[size_t(pi)].note, join_tags(a->pois[size_t(pi)].tags)};
        d.extra = "Delete";
        d.on_ok = [this, id, pi](const std::vector<std::string>& v) { doc.edit_poi(id, pi, trim(v[0]), split_tags(v[1])); };
        d.on_extra = [this, id, pi]() { doc.delete_poi(id, pi); say("Marker deleted"); };
    } else {
        d.title = "New point of interest";
        d.values = {"", ""};
        d.on_ok = [this, id, cx, cy](const std::vector<std::string>& v) {
            int i = doc.find(id);
            if (i < 0) return;
            doc.begin("add marker");
            doc.touch_area(id);
            EditArea& ar = doc.areas[size_t(i)];
            if (!ar.at(cx, cy)) {
                AreaCell fc;
                fc.height = uint8_t(std::clamp(height, 0, 35));
                ar.set(cx, cy, fc);  // a marker stands on floor
            }
            AreaPoi p;
            p.id = ar.next_poi_id();
            p.x = cx;
            p.y = cy;
            p.note = trim(v[0]);
            p.tags = split_tags(v[1]);
            ar.pois.push_back(p);
            doc.commit();
            say("Marker " + p.id + " added");
        };
    }
    open_dialog(std::move(d));
}

void EditMode::back() {
    if (dialog_) { dialog_.reset(); return; }
    if (help) { help = false; return; }
    if (!autosave_ && dirty()) {
        Dialog d;
        d.title = "Save your changes before leaving?";
        d.confirm = true;
        d.ok = "Save";
        d.extra = "Discard";
        d.on_ok = [this](const std::vector<std::string>&) { save(false); if (!dirty()) exit_ = true; };
        d.on_extra = [this]() { exit_ = true; };
        open_dialog(std::move(d));
        return;
    }
    if (autosave_ && dirty()) save(false);
    exit_ = true;
}

// --- Input ---------------------------------------------------------------------------------

bool EditMode::ui_hit(int u, int v) const {
    for (const Button& b : buttons_)
        if (u >= b.x && v >= b.y && u < b.x + b.w && v < b.y + b.h) return true;
    if (v < top_h_ || v >= bottom_y_) return true;
    if ((panel || layers_panel) && u >= panel_x_) return true;
    return false;
}

void EditMode::handle(const InputEvent& ev) {
    if (!loaded_) return;
    auto pos = [&](double& u, double& v) {
        u = (ev.fx >= 0 ? ev.fx : ev.x + 0.5) * S_;
        v = (ev.fy >= 0 ? ev.fy : ev.y + 0.5) * S_;
    };
    switch (ev.type) {
    case InputEvent::kKeyDown:
    case InputEvent::kKeyUp:
    case InputEvent::kText:
        if (dialog_) { dialog_key(ev); return; }
        key(ev);
        return;
    case InputEvent::kMouseMove: {
        double u, v;
        pos(u, v);
        mouse_move(int(u), int(v));
        mouse_u_ = u;
        mouse_v_ = v;
        return;
    }
    case InputEvent::kMouseDown: {
        double u, v;
        pos(u, v);
        mouse_u_ = u;
        mouse_v_ = v;
        mouse_down(int(u), int(v), ev.button, ev.mods);
        return;
    }
    case InputEvent::kMouseUp: {
        double u, v;
        pos(u, v);
        mouse_u_ = u;
        mouse_v_ = v;
        mouse_up(int(u), int(v), ev.button);
        return;
    }
    case InputEvent::kWheel:
        if (dialog_) return;
        set_level(level_ + (ev.wheel > 0 ? -1 : 1), mouse_u_, mouse_v_);
        return;
    case InputEvent::kPadDown:
    case InputEvent::kPadUp:
    case InputEvent::kPadAxis:
        pad(ev);
        return;
    default: return;
    }
}

void EditMode::mouse_down(int u, int v, int button, unsigned mods) {
    shift_ = mods & kModShift;
    down_u_ = last_u_ = u;
    down_v_ = last_v_ = v;
    drag_button_ = button;
    if (dialog_) {
        for (const Button& b : buttons_)
            if (u >= b.x && v >= b.y && u < b.x + b.w && v < b.y + b.h && b.enabled && b.act) { b.act(); break; }
        drag_ = Drag::kUi;
        return;
    }
    if (ui_hit(u, v)) {
        drag_ = Drag::kUi;
        if (button != 1) return;
        for (size_t i = 0; i < buttons_.size(); ++i) {
            const Button& b = buttons_[i];
            if (u >= b.x && v >= b.y && u < b.x + b.w && v < b.y + b.h) {
                if (b.enabled && b.act) {
                    auto act = b.act;  // the list is rebuilt by some actions
                    act();
                } else if (!b.tip.empty()) {
                    say(b.tip);
                }
                break;
            }
        }
        return;
    }
    if (button == 2 || button == 3) { drag_ = Drag::kPan; return; }
    if (button != 1) return;
    if (help) { help = false; drag_ = Drag::kUi; return; }
    if ((mods & (kModCtrl | kModAlt)) || tool == Tool::kMove || tool == Tool::kSegments) {
        if (show_spots) { say(std::string("The ") + reference_name(show_spots) + " spots view is read only (turn it off to move segments)"); drag_ = Drag::kPan; return; }
        if (tool == Tool::kSegments && (mods & kModShift)) {
            // Shift+click: add / remove a segment to the selection; on empty ground: a box.
            int gi = group_under(u, v);
            if (gi >= 0) {
                if (gi == selected_group) {
                    selected_group = multi.empty() ? -1 : *multi.begin();
                    multi.erase(selected_group);
                } else if (multi.count(gi)) multi.erase(gi);
                else if (selected_group < 0) selected_group = gi;
                else multi.insert(gi);
                selected.clear();
                say(std::to_string(selection().size()) + " segments selected (Shift+click adds / removes; Layers panel: reset)");
                drag_ = Drag::kUi;
                return;
            }
            box_[0] = box_[2] = u; box_[1] = box_[3] = v;
            drag_ = Drag::kBox;
            shift_box_ = true;  // adds to the selection
            return;
        }
        int ai = area_under(u, v);
        if (ai >= 0) {
            select_area(doc.areas[size_t(ai)].id);
            doc.begin("move area");
            doc.touch_area(selected);
            drag_ = Drag::kMoveArea;
        } else {
            int gi = group_under(u, v);
            if (gi >= 0) {
                if (!multi.count(gi)) multi.clear();
                start_group_drag(gi);
            } else if (tool == Tool::kSegments) {
                // Empty ground: a box selects every segment it touches.
                box_[0] = box_[2] = u; box_[1] = box_[3] = v;
                drag_ = Drag::kBox;
                shift_box_ = false;
            } else {
                selected_group = -1;
                drag_ = Drag::kPan;
            }
        }
        move_acc_[0] = move_acc_[1] = 0;
        moved_[0] = moved_[1] = 0;
        return;
    }
    switch (tool) {
    case Tool::kBrush:
    case Tool::kErase: {
        int cx, cy;
        EditArea* a = target_area(u, v, tool == Tool::kBrush, &cx, &cy);
        if (!a) { drag_ = Drag::kPan; return; }
        doc.begin(tool == Tool::kErase ? "erase" : std::string("paint ") + (kind == kHeightOnly ? "height" : cell::name(kind)));
        doc.touch_area(a->id);
        stroke_cx_ = cx;
        stroke_cy_ = cy;
        stroke_to(cx, cy);
        drag_ = Drag::kStroke;
        return;
    }
    case Tool::kLine:
    case Tool::kRect: {
        int cx, cy;
        EditArea* a = target_area(u, v, true, &cx, &cy);
        if (!a) return;
        start_cx_ = stroke_cx_ = cx;
        start_cy_ = stroke_cy_ = cy;
        drag_ = Drag::kShape;
        return;
    }
    case Tool::kFill: {
        int cx, cy;
        EditArea* a = target_area(u, v, true, &cx, &cy);
        if (!a) return;
        doc.begin("fill");
        doc.touch_area(a->id);
        int n = flood_fill(*a, cx, cy, value(false), 1);
        doc.commit();
        say(n ? "Filled " + std::to_string(n) + " cells" : "Nothing to fill there");
        drag_ = Drag::kUi;
        return;
    }
    case Tool::kMarker:
        marker_at(u, v);
        drag_ = Drag::kUi;
        return;
    default: return;
    }
}

void EditMode::mouse_move(int u, int v) {
    double du = u - last_u_, dv = v - last_v_;
    last_u_ = u;
    last_v_ = v;
    const double s = std::ldexp(1.0, level_);
    if (drag_ == Drag::kNone) hover_group_ = ui_hit(u, v) || area_under(u, v) >= 0 ? -1 : group_under(u, v);
    switch (drag_) {
    case Drag::kPan:
        cx_ -= du * s;
        cy_ -= dv * s;
        break;
    case Drag::kStroke: {
        EditArea* a = sel();
        if (!a) break;
        int cx, cy;
        cell_under(*a, u, v, cx, cy);
        if (cx != stroke_cx_ || cy != stroke_cy_) stroke_to(cx, cy);
        break;
    }
    case Drag::kShape: {
        EditArea* a = sel();
        if (a) cell_under(*a, u, v, stroke_cx_, stroke_cy_);
        break;
    }
    case Drag::kBox:
        box_[2] = u;
        box_[3] = v;
        break;
    case Drag::kMoveGroup: {
        double nsx = du * s, nsy = dv * s;
        move_acc_[0] += 2 * nsx + 4 * nsy;
        move_acc_[1] += 4 * nsy - 2 * nsx;
        group_drag_update();
        break;
    }
    case Drag::kMoveArea: {
        // Screen delta to world: x - y = 4 sx, x + y = 8 sy; snapped to whole cells (128).
        double nsx = du * s, nsy = dv * s;
        move_acc_[0] += 2 * nsx + 4 * nsy;
        move_acc_[1] += 4 * nsy - 2 * nsx;
        int snap = shift_ ? 8 : 128;
        int tx = int(std::lround(move_acc_[0] / snap)) * snap, ty = int(std::lround(move_acc_[1] / snap)) * snap;
        int ddx = tx - moved_[0], ddy = ty - moved_[1];
        if (ddx || ddy) {
            if (drag_ == Drag::kMoveArea) {
                if (EditArea* a = sel()) { a->origin[0] += ddx; a->origin[1] += ddy; }
            } else if (selected_group >= 0) {
                for (int gi : seam_cluster(selected_group)) {
                    AtlasGroup& g = doc.layout.atlas.groups[size_t(gi)];
                    g.dx += ddx;
                    g.dy += ddy;
                }
            }
            moved_[0] = tx;
            moved_[1] = ty;
        }
        break;
    }
    default: break;
    }
}

void EditMode::mouse_up(int u, int v, int button) {
    Drag d = drag_;
    drag_ = Drag::kNone;
    switch (d) {
    case Drag::kStroke: doc.commit(); break;
    case Drag::kShape: {
        EditArea* a = sel();
        if (!a) break;
        cell_under(*a, u, v, stroke_cx_, stroke_cy_);
        doc.begin(tool == Tool::kLine ? "line" : "rectangle");
        doc.touch_area(a->id);
        bool outline = (kind == cell::kWall) != shift_;
        auto cells = tool == Tool::kLine ? line_cells(brush, brush_size, start_cx_, start_cy_, stroke_cx_, stroke_cy_)
                                         : rect_cells(start_cx_, start_cy_, stroke_cx_, stroke_cy_, outline);
        paint_cells(*a, cells, value(false));
        doc.commit();
        break;
    }
    case Drag::kMoveArea:
        doc.commit();
        if (moved_[0] || moved_[1]) say("Moved by " + std::to_string(moved_[0]) + "," + std::to_string(moved_[1]));
        break;
    case Drag::kMoveGroup:
        finish_group_drag();
        break;
    case Drag::kBox: {
        if (std::abs(box_[2] - box_[0]) < 3 && std::abs(box_[3] - box_[1]) < 3) {
            if (!shift_box_) { selected_group = -1; multi.clear(); }
            shift_box_ = true;
            break;
        }
        int n = select_box(box_[0], box_[1], box_[2], box_[3], shift_box_);
        say(std::to_string(n) + " segments in the box; " + std::to_string(selection().size()) + " selected (Layers panel: reset)");
        shift_box_ = true;
        break;
    }
    default: break;
    }
}

void EditMode::key(const InputEvent& ev) {
    auto held = [&](int k, bool down) {
        switch (k) {
        case kKeyUp: held_[0] = down; break;
        case kKeyDown: held_[1] = down; break;
        case kKeyLeft: held_[2] = down; break;
        case kKeyRight: held_[3] = down; break;
        default: break;
        }
    };
    if (ev.type == InputEvent::kText) return;
    fast_ = ev.mods & kModShift;
    shift_ = ev.mods & kModShift;
    if (ev.type == InputEvent::kKeyUp) { held(ev.key, false); return; }
    held(ev.key, true);
    const bool ctrl = ev.mods & kModCtrl;
    if (ev.key == '=' || ev.key == '+') { set_level(level_ - 1, W_ / 2.0, H_ / 2.0); return; }
    if (ev.key == '-') { set_level(level_ + 1, W_ / 2.0, H_ / 2.0); return; }
    if (ev.repeat && ev.key != 'z' && ev.key != 'y') return;
    if (ctrl) {
        if (ev.key == 'z' && (ev.mods & kModShift)) { if (doc.redo()) say("Redo"); }
        else if (ev.key == 'z') { if (doc.undo()) say("Undo"); }
        else if (ev.key == 'y') { if (doc.redo()) say("Redo"); }
        else if (ev.key == 's') save(false);
        // Ctrl+arrows nudge the selected group / area (groups 512, Shift 64; areas 128, Shift 8);
        // Ctrl+PgUp / PgDn move it a layer up / down.
        else if (ev.key == kKeyUp || ev.key == kKeyDown || ev.key == kKeyLeft || ev.key == kKeyRight) {
            int step = sel() ? (shift_ ? 8 : 128) : (shift_ ? 64 : 512);
            // Screen directions: up = -x -y, right = +x -y (isometric).
            int dx = 0, dy = 0;
            if (ev.key == kKeyUp) { dx = -step; dy = -step; }
            if (ev.key == kKeyDown) { dx = step; dy = step; }
            if (ev.key == kKeyLeft) { dx = -step; dy = step; }
            if (ev.key == kKeyRight) { dx = step; dy = -step; }
            held(ev.key, false);  // (not a pan)
            if (!nudge_selected(dx, dy)) say("Select a map group or an area first (Move tool, or the Layers panel: Y)");
            return;
        } else if (ev.key == kKeyPageUp || ev.key == kKeyPageDown) {
            if (!selected_layer_step(ev.key == kKeyPageUp ? 1 : -1)) say("Select a map group or an area first");
            return;
        }
        refresh_exclusions();
        return;
    }
    switch (ev.key) {
    case kKeyEscape:
        if (drag_ != Drag::kNone) {
            if (doc.in_change()) doc.cancel();
            drag_ = Drag::kNone;
        } else if (help) help = false;
        else if (panel) panel = false;
        else if (layers_panel) layers_panel = false;
        else back();
        break;
    case kKeyPageUp: ++layer; say("Layer " + std::to_string(layer)); break;
    case kKeyPageDown: --layer; say("Layer " + std::to_string(layer)); break;
    case kKeyHome: show_world_point(16384 + 12000, 16384 + 12000, 7); break;
    case kKeyTab: panel = !panel; if (panel) layers_panel = false; break;
    case 'y': if (!ev.repeat) { layers_panel = !layers_panel; if (layers_panel) panel = false; } break;
    case kKeyF1: help = !help; break;
    case kKeyDelete:
        if (EditArea* a = sel()) {
            std::string id = a->id;
            Dialog d;
            d.title = "Delete area \"" + a->name + "\"?";
            d.confirm = true;
            d.ok = "Delete";
            d.on_ok = [this, id](const std::vector<std::string>&) { doc.delete_area(id); selected.clear(); say("Deleted (Ctrl+Z brings it back)"); };
            open_dialog(std::move(d));
        }
        break;
    case '1': kind = cell::kFloor; break;
    case '2': kind = cell::kWall; break;
    case '3': kind = cell::kPassage; break;
    case '4': kind = cell::kDoor; break;
    case '5': kind = cell::kWater; break;
    case '6': kind = cell::kChasm; break;
    case '7': kind = cell::kStairs; break;
    case '8': kind = kHeightOnly; break;
    case 'b': tool = Tool::kBrush; break;
    case 's': tool = Tool::kSegments; break;
    case 'n': {
        static const int steps[] = {512, 256, 128, 64, 0};
        int i = 0;
        while (i < 5 && steps[i] != snap_grid) ++i;
        snap_grid = steps[(i + 1) % 5];
        say(snap_grid ? "Snap " + std::to_string(snap_grid) : std::string("Snap off"));
        break;
    }
    case 'j': move_joined = !move_joined; say(move_joined ? "Joined pieces move together" : "Only the grabbed segment moves"); break;
    case 'l': tool = Tool::kLine; break;
    case 'r': tool = Tool::kRect; break;
    case 'f': tool = Tool::kFill; break;
    case 'e': tool = Tool::kErase; break;
    case 'p': tool = Tool::kMarker; break;
    case 'm': tool = Tool::kMove; break;
    case 'o': brush = brush == BrushShape::kSquare ? BrushShape::kRound : BrushShape::kSquare; break;
    case '[': brush_size = std::max(1, brush_size - 1); break;
    case ']': brush_size = std::min(16, brush_size + 1); break;
    case ',': height = std::max(0, height - 1); say("Height " + std::to_string(height)); break;
    case '.': height = std::min(35, height + 1); say("Height " + std::to_string(height)); break;
    case 'g': show_grid = !show_grid; break;
    case 'd':
        if (dressed_) { dressed_.reset(); say("Back to the paint"); }
        else dress();
        publish_state();
        break;
    case 'h': help = !help; break;
    default: break;
    }
}

void EditMode::dialog_key(const InputEvent& ev) {
    Dialog& d = *dialog_;
    if (ev.type == InputEvent::kText) {
        if (d.confirm || d.values.empty()) return;
        std::string& s = d.values[size_t(d.focus)];
        for (const char* p = ev.text; *p; ++p)
            if (static_cast<unsigned char>(*p) >= 32 && static_cast<unsigned char>(*p) < 127 && s.size() < 200) s += *p;
        return;
    }
    if (ev.type != InputEvent::kKeyDown) return;
    if (ev.key == kKeyEscape) { dialog_.reset(); return; }
    if (ev.key == kKeyReturn) {
        auto ok = d.on_ok;
        auto vals = d.values;
        dialog_.reset();
        if (ok) ok(vals);
        return;
    }
    if (d.values.empty()) return;
    if (ev.key == kKeyTab || ev.key == kKeyDown || ev.key == kKeyUp) {
        int n = int(d.values.size());
        d.focus = (d.focus + (ev.key == kKeyUp || (ev.mods & kModShift) ? n - 1 : 1)) % n;
    } else if (ev.key == kKeyBackspace) {
        std::string& s = d.values[size_t(d.focus)];
        if (!s.empty()) s.pop_back();
    }
}

void EditMode::pad(const InputEvent& ev) {
    if (ev.type == InputEvent::kPadAxis) {
        if (ev.axis >= 0 && ev.axis < 6) pad_axis_[ev.axis] = ev.value / 32768.0;
        if (ev.axis == kAxisLT && ev.value > 20000) set_level(level_ + 1, mouse_u_, mouse_v_);
        if (ev.axis == kAxisRT && ev.value > 20000) set_level(level_ - 1, mouse_u_, mouse_v_);
        return;
    }
    const bool down = ev.type == InputEvent::kPadDown;
    if (ev.key == kPadA) {
        if (down && !pad_a_) mouse_down(int(mouse_u_), int(mouse_v_), 1, 0);
        if (!down && pad_a_) mouse_up(int(mouse_u_), int(mouse_v_), 1);
        pad_a_ = down;
        return;
    }
    if (!down) return;
    if (dialog_) {
        if (ev.key == kPadB) dialog_.reset();
        return;
    }
    static const char kinds[] = {cell::kFloor, cell::kWall, cell::kPassage, cell::kDoor, cell::kWater, cell::kChasm, cell::kStairs, kHeightOnly};
    int ki = 0;
    for (int i = 0; i < 8; ++i)
        if (kinds[i] == kind) ki = i;
    switch (ev.key) {
    case kPadB: back(); break;
    case kPadX: if (doc.undo()) { say("Undo"); refresh_exclusions(); } break;
    case kPadY: if (doc.redo()) { say("Redo"); refresh_exclusions(); } break;
    case kPadLB: kind = kinds[(ki + 7) % 8]; break;
    case kPadRB: kind = kinds[(ki + 1) % 8]; break;
    case kPadUp: ++layer; say("Layer " + std::to_string(layer)); break;
    case kPadDown: --layer; say("Layer " + std::to_string(layer)); break;
    case kPadLeft: brush_size = std::max(1, brush_size - 1); break;
    case kPadRight: brush_size = std::min(16, brush_size + 1); break;
    case kPadStart: save(false); break;
    case kPadBack: tool = Tool((int(tool) + 1) % 7); break;
    case kPadRStick: panel = !panel; break;
    default: break;
    }
}

void EditMode::tick() {
    ++ticks_;
    if (!loaded_) { load_all(); return; }
    const double s = std::ldexp(1.0, level_);
    double step = (fast_ ? 24 : 6) * S_ * s;
    if (held_[0]) cy_ -= step;
    if (held_[1]) cy_ += step;
    if (held_[2]) cx_ -= step;
    if (held_[3]) cx_ += step;
    // Controller: the left stick moves the pointer, the right stick pans.
    auto dz = [](double a) { return std::fabs(a) < 0.25 ? 0.0 : (a - (a > 0 ? 0.25 : -0.25)) / 0.75; };
    double lx = dz(pad_axis_[kAxisLX]), ly = dz(pad_axis_[kAxisLY]), rx = dz(pad_axis_[kAxisRX]), ry = dz(pad_axis_[kAxisRY]);
    if (lx || ly) {
        mouse_u_ = std::clamp(mouse_u_ + lx * 5 * S_, 0.0, double(W_ - 1));
        mouse_v_ = std::clamp(mouse_v_ + ly * 5 * S_, 0.0, double(H_ - 1));
        mouse_move(int(mouse_u_), int(mouse_v_));
    }
    if (rx || ry) { cx_ += rx * 8 * S_ * s; cy_ += ry * 8 * S_ * s; }
    if (message_ticks > 0) --message_ticks;
    autosave_tick();
#ifdef __EMSCRIPTEN__
    // Upload: the page's file picker leaves the text in Module.pgEditUpload.
    if (ticks_ % 10 == 0) {
        char* p = (char*)EM_ASM_PTR({
            try {
                if (!Module.pgEditUpload) return 0;
                const s = Module.pgEditUpload;
                Module.pgEditUpload = null;
                return stringToNewUTF8(s);
            } catch (e) { return 0; }
        });
        if (p) {
            import_text(p);
            std::free(p);
        }
    }
#endif
}

// --- UI ------------------------------------------------------------------------------------

void EditMode::layout_ui() {
    buttons_.clear();
    const ShapeFont& f = fonts_.size() > 6 ? fonts_[6] : fonts_[0];
    const int u = ui_;
    const int bh = (f.line_advance() + 5) * u, gap = 2 * u;
    int x = 3 * u, y = 3 * u;
    auto add = [&](const std::string& label, std::function<void()> act, bool on = false, bool enabled = true,
                   const std::string& tip = "", uint32_t swatch = 0) -> Button& {
        Button b;
        b.label = label;
        b.w = (f.text_width(label) + 8) * u + (swatch ? 9 * u : 0);
        b.h = bh;
        if (x + b.w > W_ - 3 * u) { x = 3 * u; y += bh + gap; }
        b.x = x;
        b.y = y;
        b.act = std::move(act);
        b.on = on;
        b.enabled = enabled;
        b.tip = tip;
        b.swatch = swatch;
        x += b.w + gap;
        buttons_.push_back(b);
        return buttons_.back();
    };
    auto sep = [&]() { x += 5 * u; };
    if (dialog_) {
        // Only the dialog's own buttons (laid out in draw_dialog's box).
        Dialog& d = *dialog_;
        int dw = std::min(W_ - 20 * u, 300 * u), lines = d.confirm ? 1 : int(d.values.size()) * 2;
        int dh = (lines + 4) * (f.line_advance() + 4) * u;
        int dx = (W_ - dw) / 2, dy = (H_ - dh) / 2;
        x = dx + 8 * u;
        y = dy + dh - bh - 6 * u;
        add(d.ok, [this]() {
            auto ok = dialog_->on_ok;
            auto vals = dialog_->values;
            dialog_.reset();
            if (ok) ok(vals);
        });
        if (!d.extra.empty())
            add(d.extra, [this]() {
                auto ex = dialog_->on_extra;
                dialog_.reset();
                if (ex) ex();
            });
        add("Cancel", [this]() { dialog_.reset(); });
        // The fields are buttons too (a click focuses).
        for (size_t i = 0; i < d.values.size(); ++i) {
            Button b;
            b.x = dx + 8 * u;
            b.y = dy + (int(i) * 2 + 2) * (f.line_advance() + 4) * u;
            b.w = dw - 16 * u;
            b.h = (f.line_advance() + 4) * u;
            b.label = "\x01field";
            b.act = [this, i]() { if (dialog_) dialog_->focus = int(i); };
            buttons_.push_back(b);
        }
        top_h_ = 0;
        bottom_y_ = H_;
        return;
    }
    struct K { char k; const char* name; };
    static const K kinds[] = {{cell::kFloor, "Floor"}, {cell::kWall, "Wall"}, {cell::kPassage, "Passage"}, {cell::kDoor, "Door"},
                              {cell::kWater, "Water"}, {cell::kChasm, "Chasm"}, {cell::kStairs, "Stairs"}, {kHeightOnly, "Height"}};
    int n = 1;
    for (const K& k : kinds) {
        char kk = k.k;
        add(k.name, [this, kk]() { kind = kk; if (tool == Tool::kErase || tool == Tool::kMove || tool == Tool::kMarker || tool == Tool::kSegments) tool = Tool::kBrush; },
            kind == kk, true, std::string("Key ") + char('0' + n), kind_color(kk));
        ++n;
    }
    sep();
    struct T { Tool t; const char* name; const char* tip; };
    static const T tools[] = {{Tool::kSegments, "Segments", "S: grab a segment (a map or a painted area) and drag it; Alt+drag with any tool"},
                              {Tool::kBrush, "Brush", "B: paint with the brush"},
                              {Tool::kLine, "Line", "L: drag a line"},
                              {Tool::kRect, "Rect", "R: drag a rectangle (walls: outline; Shift swaps)"},
                              {Tool::kFill, "Fill", "F: flood fill"},
                              {Tool::kErase, "Erase", "E: erase with the brush"},
                              {Tool::kMarker, "Marker", "P: point of interest (click a marker to edit it)"},
                              {Tool::kMove, "Move", "M: drag an area or a map group (Ctrl+drag with any tool)"}};
    for (const T& t : tools) {
        Tool tt = t.t;
        add(t.name, [this, tt]() { tool = tt; }, tool == tt, true, t.tip);
    }
    sep();
    add(brush == BrushShape::kSquare ? "Square" : "Round", [this]() { brush = brush == BrushShape::kSquare ? BrushShape::kRound : BrushShape::kSquare; },
        false, true, "O: brush shape");
    add("-", [this]() { brush_size = std::max(1, brush_size - 1); }, false, true, "[: smaller brush");
    add("Size " + std::to_string(brush_size), nullptr, false, false, "Brush size ([ and ])");
    add("Bigger", [this]() { brush_size = std::min(16, brush_size + 1); }, false, true, "]: bigger brush");
    sep();
    add("Undo", [this]() { if (doc.undo()) { say("Undo " + doc.redo_label()); refresh_exclusions(); } }, false, doc.can_undo(),
        doc.can_undo() ? "Ctrl+Z: undo " + doc.undo_label() : "Ctrl+Z: nothing to undo");
    add("Redo", [this]() { if (doc.redo()) { say("Redo " + doc.undo_label()); refresh_exclusions(); } }, false, doc.can_redo(),
        doc.can_redo() ? "Ctrl+Y / Ctrl+Shift+Z: redo " + doc.redo_label() : "Ctrl+Y: nothing to redo");
    sep();
    add(dirty() ? "Save*" : "Save", [this]() { save(false); }, false, storage_ != nullptr, "Ctrl+S");
    add("Download", [this]() { export_bundle(); }, false, true, "All areas + the layout as one JSON file");
    add("Upload", [this]() { import_bundle(); }, false, true, "Load a downloaded JSON file");
    sep();
    add(dressed_ ? "Undress" : "Dress", [this]() { if (dressed_) { dressed_.reset(); say("Back to the paint"); } else dress(); },
        dressed_ != nullptr, dresser_available() && sel(),
        dresser_available() ? "Select an area to dress" : "Dress: the map dresser is not built in yet (coming with section 3)");
    add("Re-roll", [this]() { reroll(); }, false, sel() != nullptr, "New seed for the selected area");
    sep();
    add("Areas", [this]() { panel = !panel; if (panel) layers_panel = false; }, panel, true, "Tab");
    add("Layers", [this]() { layers_panel = !layers_panel; if (layers_panel) panel = false; }, layers_panel, true,
        "Y: every layer (show / hide), its map groups and areas; select, move, change layer");
    add("Help", [this]() { help = !help; }, help, true, "F1");
    add(from_game_ ? "Back" : "Quit", [this]() { back(); }, false, true, "Esc");
    top_h_ = y + bh + 3 * u;

    // Bottom bar (laid out from the bottom).
    int by = H_ - bh - 3 * u;
    bottom_y_ = by - 3 * u;
    x = 3 * u;
    y = by;
    add("Below", [this]() { --layer; }, false, true, "PgDn: the layer below");
    add("Layer " + std::to_string(layer), nullptr, false, false, "Layers above / below are ghosted");
    add("Above", [this]() { ++layer; }, false, true, "PgUp: the layer above");
    sep();
    add("Lower", [this]() { height = std::max(0, height - 1); }, false, true, ",: lower");
    add("Height " + std::to_string(height), nullptr, false, false, "Paint height level (z = level x 8)");
    add("Higher", [this]() { height = std::min(35, height + 1); }, false, true, ".: higher");
    sep();
    const auto& th = themes();
    auto ti = std::find(th.begin(), th.end(), theme);
    int tix = ti == th.end() ? 0 : int(ti - th.begin());
    add("Prev", [this, tix]() {
        const auto& t = themes();
        theme = t[size_t((tix + int(t.size()) - 1) % int(t.size()))];
        if (EditArea* a = sel()) doc.set_area_theme(a->id, theme);
    }, false, true, "Theme of the selected area");
    add(theme, nullptr, false, false, "Theme");
    add("Next", [this, tix]() {
        const auto& t = themes();
        theme = t[size_t((tix + 1) % int(t.size()))];
        if (EditArea* a = sel()) doc.set_area_theme(a->id, theme);
    }, false, true, "Theme of the selected area");
    sep();
    add("Grid", [this]() { show_grid = !show_grid; }, show_grid, true, "G");
    sep();
    add(snap_grid ? "Snap " + std::to_string(snap_grid) : std::string("Snap off"), [this]() {
        static const int steps[] = {512, 256, 128, 64, 0};
        int i = 0;
        while (i < 5 && steps[i] != snap_grid) ++i;
        snap_grid = steps[(i + 1) % 5];
        say(snap_grid ? "Segments snap to a " + std::to_string(snap_grid) + " grid (and to exact seam / teleporter offsets nearby)"
                      : std::string("Snap off (Shift while dragging: no seam snap either)"));
    }, snap_grid != 0, true, "N: segment snap grid 512 / 256 / 128 / 64 / off; exact seam and teleporter offsets snap too");
    add(move_joined ? "Joined: move together" : "Joined: one only", [this]() {
        move_joined = !move_joined;
        say(move_joined ? "Segments joined by seams move together" : "Only the grabbed segment moves (moving off a seam breaks it)");
    }, move_joined, true, "J: move joined pieces (seam partners) together");
    add("Show Pagan spots", [this]() {
        show_spots = show_spots == kPagan ? 0 : kPagan;
        refresh_exclusions();
        say(show_spots ? "Every segment where the original Ultima 8 had it (view only)" : "Segments at their layout places");
    }, show_spots == kPagan, has_reference(kPagan), "Show every segment at its Pagan spot (the original placement); view only");
    add("Show Baseline spots", [this]() {
        show_spots = show_spots == kBaseline ? 0 : kBaseline;
        refresh_exclusions();
        say(show_spots ? "Every segment where the Baseline has it (view only)" : "Segments at their layout places");
    }, show_spots == kBaseline, has_reference(kBaseline), "Show every segment at its Baseline spot (place and layer); view only");
    if (y != by) {  // wrapped: move the bar up so it fits
        int extra = y - by;
        for (size_t i = 0; i < buttons_.size(); ++i)
            if (buttons_[i].y >= by - 1) buttons_[i].y -= extra;
        bottom_y_ -= extra;
    }

    // Layers panel.
    if (layers_panel) {
        int pw = std::min(W_ / 2, 200 * u);
        panel_x_ = W_ - pw;
        x = panel_x_ + 4 * u;
        y = top_h_ + 4 * u + (f.line_advance() + 4) * u;
        auto row = [&]() { x = panel_x_ + 4 * u; y += bh + gap; };
        auto padd = [&](const std::string& label, std::function<void()> act, bool on = false, bool enabled = true, const std::string& tip = "") {
            std::string l = label;
            int room_w = W_ - 4 * u - x - gap;  // what is left of this row
            if (room_w < 40 * u) room_w = pw - 10 * u;
            while (l.size() > 6 && (f.text_width(l) + 8) * u > room_w) l = l.substr(0, l.size() - 3) + "..";
            Button& b = add(l, std::move(act), on, enabled, tip);
            if (b.x < panel_x_) {
                row();
                b.x = x;
                b.y = y;
                x += b.w + gap;
            }
        };
        padd("Reset all to Pagan", [this]() { reset_all(kPagan); }, false, has_reference(kPagan),
             "Every segment to where the original Ultima 8 had it (x / y; undoable)");
        row();
        padd("Reset all to Baseline", [this]() { reset_all(kBaseline); }, false, has_reference(kBaseline),
             "Every segment, seam and link as in the Baseline (undoable)");
        row();
        for (int L : layers_in_use()) {
            int ng = 0, na = 0;
            for (const AtlasGroup& g : doc.layout.atlas.groups) ng += g.layer == L;
            for (const EditArea& a : doc.areas) na += a.layer == L;
            bool shown = !hidden_layers.count(L);
            padd(shown ? "On" : "Off", [this, L]() {
                if (hidden_layers.count(L)) hidden_layers.erase(L);
                else if (L != layer) hidden_layers.insert(L);
                else say("The current layer is always shown");
            }, shown, true, "Show / hide this layer (its ghost too)");
            std::string nm = layer_name(L);
            padd("L" + std::to_string(L) + " z" + std::to_string(L * kLayerZ) + "  " + std::to_string(ng) + "g " + std::to_string(na) + "a" +
                     (nm.empty() ? "" : "  " + nm),
                 [this, L]() { layer = L; hidden_layers.erase(L); say("Layer " + std::to_string(L) + (layer_name(L).empty() ? "" : ": " + layer_name(L))); },
                 L == layer, true, nm.empty() ? "Go to this layer" : nm);
            row();
        }
        y += gap;
        // What is on the current layer: click = select and centre (then drag with Move, or the buttons below).
        std::vector<std::pair<std::string, std::function<void()>>> items;
        std::vector<bool> on;
        for (size_t gi = 0; gi < doc.layout.atlas.groups.size(); ++gi) {
            const AtlasGroup& g = doc.layout.atlas.groups[gi];
            if (g.layer != layer) continue;
            bool unplaced = g.extra.find("\"unplaced\":true") != std::string::npos;
            int i = int(gi);
            bool tagged = g.label.find("(unplaced)") != std::string::npos;
            items.push_back({(unplaced && !tagged ? "UNPLACED: " : "") + g.label, [this, i]() {
                if (shift_ && selected_group >= 0 && selected_group != i) {  // Shift+click: add / remove
                    if (multi.count(i)) multi.erase(i);
                    else multi.insert(i);
                    say(std::to_string(selection().size()) + " segments selected");
                } else {
                    multi.clear();
                    select_group(i);
                }
            }});
            on.push_back(selected_group == i || multi.count(i) > 0);
        }
        for (const EditArea& a : doc.areas) {
            if (a.layer != layer) continue;
            std::string id = a.id;
            items.push_back({"Area: " + (a.name.empty() ? a.id : a.name), [this, id]() {
                select_area(id);
                if (EditArea* e = sel()) {
                    int x0, y0, x1, y1;
                    if (e->bounds(x0, y0, x1, y1)) show_world_point(e->origin[0] + (x0 + x1 + 1) * e->cell / 2, e->origin[1] + (y0 + y1 + 1) * e->cell / 2, level_);
                    else show_world_point(e->origin[0], e->origin[1], level_);
                }
            }});
            on.push_back(a.id == selected);
        }
        int room = std::max(1, (bottom_y_ - 10 * (bh + gap) - y) / (bh + gap));
        layers_scroll_ = std::clamp(layers_scroll_, 0, std::max(0, int(items.size()) - room));
        if (layers_scroll_ > 0 || int(items.size()) > room) {
            padd("Prev", [this]() { layers_scroll_ = std::max(0, layers_scroll_ - 4); }, false, layers_scroll_ > 0, "Scroll the list");
            padd("Next", [this]() { layers_scroll_ += 4; }, false, layers_scroll_ + room < int(items.size()), "Scroll the list");
            padd(std::to_string(items.size()) + " on this layer", nullptr, false, false);
            row();
            room = std::max(1, room - 1);
        }
        for (int i = layers_scroll_; i < int(items.size()) && i < layers_scroll_ + room; ++i) {
            padd(items[size_t(i)].first, items[size_t(i)].second, on[size_t(i)], true, "Select and centre");
            row();
        }
        // The selection: nudge (screen directions) and layer up / down.
        bool have = sel() != nullptr || (selected_group >= 0 && size_t(selected_group) < doc.layout.atlas.groups.size());
        if (have) {
            y = std::max(y, bottom_y_ - 7 * (bh + gap));
            x = panel_x_ + 4 * u;
            std::string what;
            if (EditArea* a = sel()) what = a->name + "  L" + std::to_string(a->layer);
            else {
                const AtlasGroup& g = doc.layout.atlas.groups[size_t(selected_group)];
                size_t n = seam_cluster(selected_group).size();
                what = g.label + "  L" + std::to_string(g.layer) + " dz " + std::to_string(g.dz) + (n > 1 ? "  +" + std::to_string(n - 1) + " seam" : "");
            }
            padd(what, nullptr, true, false, "The selection (seam partners move with a map group)");
            row();
            const int st = sel() ? 128 : 512;
            padd("Up", [this, st]() { nudge_selected(-st, -st); }, false, true, "Ctrl+Up: move it up the screen");
            padd("Down", [this, st]() { nudge_selected(st, st); }, false, true, "Ctrl+Down: down the screen");
            padd("Left", [this, st]() { nudge_selected(-st, st); }, false, true, "Ctrl+Left");
            padd("Right", [this, st]() { nudge_selected(st, -st); }, false, true, "Ctrl+Right");
            row();
            padd("Layer up", [this]() { selected_layer_step(1); }, false, true, "Ctrl+PgUp: a layer up (above)");
            padd("Layer down", [this]() { selected_layer_step(-1); }, false, true, "Ctrl+PgDn: a layer down (below)");
            row();
            if (!sel()) {
                size_t n = selection().size();
                std::string what = n > 1 ? std::to_string(n) + " " : std::string();
                padd("Reset " + what + "to Pagan", [this]() { reset_selection(kPagan); }, false, has_reference(kPagan),
                     "Back to the original Ultima 8 placement (x / y; keeps the layer)");
                row();
                padd("Reset " + what + "to Baseline", [this]() { reset_selection(kBaseline); }, false, has_reference(kBaseline),
                     "Back to the Baseline (place, layer and z)");
                row();
            }
        }
    }

    // Areas panel.
    if (panel) {
        int pw = std::min(W_ / 2, 190 * u);
        panel_x_ = W_ - pw;
        x = panel_x_ + 4 * u;
        y = top_h_ + 4 * u + (f.line_advance() + 4) * u;
        auto row = [&]() { x = panel_x_ + 4 * u; y += bh + gap; };
        auto padd = [&](const std::string& label, std::function<void()> act, bool on = false, bool enabled = true, const std::string& tip = "") {
            Button& b = add(label, std::move(act), on, enabled, tip);
            if (b.x < panel_x_) {  // wrapped to the left edge: back into the panel
                row();
                b.x = x;
                b.y = y;
                x += b.w + gap;
            }
        };
        padd("New area", [this]() {
            double wx, wy;
            canvas_to_world(W_ / 2.0, H_ / 2.0, 0, wx, wy);
            int org[3] = {int(std::floor(wx / 128)) * 128, int(std::floor(wy / 128)) * 128, 0};
            EditArea& a = doc.create_area("Area " + std::to_string(doc.areas.size() + 1), org, layer, theme);
            select_area(a.id);
        });
        row();
        for (const EditArea& a : doc.areas) {
            if (y > bottom_y_ - 8 * (bh + gap)) break;
            std::string id = a.id;
            std::string lab = (a.name.empty() ? a.id : a.name) + "  L" + std::to_string(a.layer);
            padd(lab, [this, id]() {
                select_area(id);
                if (EditArea* e = sel()) {
                    int x0, y0, x1, y1;
                    if (e->bounds(x0, y0, x1, y1)) show_world_point(e->origin[0] + (x0 + x1 + 1) * e->cell / 2, e->origin[1] + (y0 + y1 + 1) * e->cell / 2, level_);
                    else show_world_point(e->origin[0], e->origin[1], level_);
                }
            }, a.id == selected);
            row();
        }
        if (EditArea* a = sel()) {
            std::string id = a->id;
            y += gap;
            padd("Rename", [this, id]() {
                Dialog d;
                d.title = "Rename area";
                d.labels = {"Name"};
                d.values = {sel() ? sel()->name : ""};
                d.on_ok = [this, id](const std::vector<std::string>& v) { if (!trim(v[0]).empty()) doc.rename_area(id, trim(v[0])); };
                open_dialog(std::move(d));
            });
            padd("Delete", [this, id]() {
                Dialog d;
                d.title = "Delete this area?";
                d.confirm = true;
                d.ok = "Delete";
                d.on_ok = [this, id](const std::vector<std::string>&) { doc.delete_area(id); selected.clear(); say("Deleted (Undo brings it back)"); };
                open_dialog(std::move(d));
            });
            row();
            padd("L-", [this, id]() { if (EditArea* e = sel()) { doc.set_area_layer(id, e->layer - 1); layer = e->layer; } }, false, true, "Move the area a layer down");
            padd("Layer " + std::to_string(a->layer), nullptr, false, false);
            padd("L+", [this, id]() { if (EditArea* e = sel()) { doc.set_area_layer(id, e->layer + 1); layer = e->layer; } }, false, true, "Move the area a layer up");
            row();
            padd("Z-", [this, id]() { doc.move_area(id, 0, 0, shift_ ? -64 : -8); }, false, true, "z offset -8 (Shift -64)");
            padd("Z " + std::to_string(a->origin[2]), nullptr, false, false, "World z offset of the area");
            padd("Z+", [this, id]() { doc.move_area(id, 0, 0, shift_ ? 64 : 8); }, false, true, "z offset +8 (Shift +64)");
            row();
            padd("Seed " + std::to_string(a->seed), nullptr, false, false);
            row();
        } else if (selected_group >= 0 && size_t(selected_group) < doc.layout.atlas.groups.size()) {
            const AtlasGroup& g = doc.layout.atlas.groups[size_t(selected_group)];
            int gi = selected_group;
            y += gap;
            padd(g.label, nullptr, true, false);
            row();
            padd("L-", [this, gi]() { selected_group = gi; selected_layer_step(-1); }, false, true, "A layer down (seam partners too)");
            padd("Layer " + std::to_string(g.layer), nullptr, false, false);
            padd("L+", [this, gi]() { selected_group = gi; selected_layer_step(1); }, false, true, "A layer up (seam partners too)");
            row();
            padd("Z-", [this, gi]() { doc.move_group(gi, 0, 0, shift_ ? -64 : -8); });
            padd("Z " + std::to_string(g.dz), nullptr, false, false);
            padd("Z+", [this, gi]() { doc.move_group(gi, 0, 0, shift_ ? 64 : 8); });
            row();
        }
    }
}

void EditMode::draw_ui(RgbaCanvas& c) {
    const ShapeFont& f = fonts_.size() > 6 ? fonts_[6] : fonts_[0];
    const int u = ui_;
    if (!dialog_) {
        c.shade(0, 0, W_, top_h_, kBarColor, 215);
        c.shade(0, bottom_y_, W_, H_ - bottom_y_, kBarColor, 215);
        if (panel) {
            c.shade(panel_x_, top_h_, W_ - panel_x_, bottom_y_ - top_h_, kBarColor, 225);
            c.text(f, panel_x_ + 5 * u, top_h_ + 4 * u, "Areas (" + std::to_string(doc.areas.size()) + ")", u);
        }
        if (layers_panel) {
            c.shade(panel_x_, top_h_, W_ - panel_x_, bottom_y_ - top_h_, kBarColor, 225);
            c.text(f, panel_x_ + 5 * u, top_h_ + 4 * u, "Layers (current: " + std::to_string(layer) + ")  - groups (g), areas (a)", u);
        }
    }
    hover_button_ = -1;
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const Button& b = buttons_[i];
        if (b.label == "\x01field") continue;
        bool hot = mouse_u_ >= b.x && mouse_v_ >= b.y && mouse_u_ < b.x + b.w && mouse_v_ < b.y + b.h;
        if (hot) hover_button_ = int(i);
        bool label_only = !b.act && !b.enabled;
        uint32_t bg = b.on ? 0xFF6A4A20u : label_only ? 0xFF1C1E28u : hot && b.enabled ? 0xFF3A3E50u : 0xFF282A36u;
        c.shade(b.x, b.y, b.w, b.h, bg, 255);
        if (!label_only) rect_outline(c, b.x, b.y, b.w, b.h, b.on ? 0xFFE0A040u : b.enabled ? 0xFF50546Au : 0xFF34363Eu);
        int tx = b.x + 4 * u;
        if (b.swatch) {
            c.shade(tx, b.y + 3 * u, 6 * u, b.h - 6 * u, b.swatch, 255);
            tx += 9 * u;
        }
        c.text(f, tx, b.y + 2 * u + u / 2, b.label, u);
        if (!b.enabled && !label_only) c.shade(b.x + 1, b.y + 1, b.w - 2, b.h - 2, 0xFF14161Eu, 140);
    }
    if (dialog_) return;
    // Status: hover tip, message, or what is under the pointer.
    std::string status;
    if (message_ticks > 0) status = message;
    else if (hover_button_ >= 0 && !buttons_[size_t(hover_button_)].tip.empty()) status = buttons_[size_t(hover_button_)].tip;
    else if (!ui_hit(int(mouse_u_), int(mouse_v_))) {
        char line[256];
        double wx, wy;
        canvas_to_world(mouse_u_, mouse_v_, height * 8, wx, wy);
        int ai = area_under(mouse_u_, mouse_v_);
        if (ai >= 0) {
            const EditArea& a = doc.areas[size_t(ai)];
            int cx, cy;
            cell_under(a, mouse_u_, mouse_v_, cx, cy);
            const AreaCell* ce = a.at(cx, cy);
            int pi = a.poi_index(cx, cy);
            std::snprintf(line, sizeof line, "%s cell %d,%d: %s, height %d%s%s", a.name.c_str(), cx, cy, ce ? cell::name(ce->kind) : "void",
                          ce ? ce->height : 0, pi >= 0 ? "  marker: " : "", pi >= 0 ? a.pois[size_t(pi)].note.c_str() : "");
        } else {
            int gi = group_under(mouse_u_, mouse_v_);
            if (gi >= 0) {
                const AtlasGroup& g = doc.layout.atlas.groups[size_t(gi)];
                std::snprintf(line, sizeof line, "%s  local %d,%d  world %d,%d", g.label.c_str(), int(wx) - g.dx, int(wy) - g.dy, int(wx), int(wy));
            } else {
                std::snprintf(line, sizeof line, "world %d,%d", int(wx), int(wy));
            }
        }
        status = line;
    }
    char zl[64];
    if (level_ >= 0) std::snprintf(zl, sizeof zl, "1:%ld", 1L << level_);
    else std::snprintf(zl, sizeof zl, "%d:1", 1 << -level_);
    std::string right = std::string(zl) + (pending_ ? "  rendering" : "") + (dirty() ? "  unsaved" : "");
    int rw = (f.text_width(right) + 6) * u;
    int sy = bottom_y_ - (f.line_advance() + 5) * u;
    int maxw = (panel ? panel_x_ : W_) - rw - 8 * u;
    while (!status.empty() && (f.text_width(status) + 6) * u > maxw) status.pop_back();
    if (!status.empty()) {
        c.shade(0, sy, (f.text_width(status) + 8) * u, (f.line_advance() + 5) * u, 0xFF000000u, 160);
        c.text(f, 4 * u, sy + 2 * u, status, u);
    }
    int rx = (panel ? panel_x_ : W_) - rw - 2 * u;
    c.shade(rx, sy, rw, (f.line_advance() + 5) * u, 0xFF000000u, 160);
    c.text(f, rx + 3 * u, sy + 2 * u, right, u);
}

void EditMode::draw_preview(RgbaCanvas& c) {
    if (dialog_ || ui_hit(int(mouse_u_), int(mouse_v_)) || tool == Tool::kMove || help) return;
    const EditArea* a = sel();
    if (a && a->layer != layer) a = nullptr;
    EditArea tmp;
    if (!a) {
        double x, y;
        canvas_to_world(mouse_u_, mouse_v_, height * 8, x, y);
        tmp.origin[0] = int(std::floor(x / 128)) * 128;
        tmp.origin[1] = int(std::floor(y / 128)) * 128;
        a = &tmp;
    }
    std::vector<std::pair<int, int>> cells;
    int cx, cy;
    cell_under(*a, mouse_u_, mouse_v_, cx, cy);
    if (drag_ == Drag::kShape)
        cells = tool == Tool::kLine ? line_cells(brush, brush_size, start_cx_, start_cy_, stroke_cx_, stroke_cy_)
                                    : rect_cells(start_cx_, start_cy_, stroke_cx_, stroke_cy_, (kind == cell::kWall) != shift_);
    else if (tool == Tool::kBrush || tool == Tool::kErase)
        cells = brush_cells(brush, brush_size, cx, cy);
    else
        cells = {{cx, cy}};
    const double C = a->cell;
    const double z = a->base_z + height * a->level_z;
    uint32_t col = tool == Tool::kErase ? 0xFFFF4040u : tool == Tool::kMarker ? 0xFFFFE060u : kind_color(kind);
    for (auto [x, y] : cells) {
        double us[4], vs[4];
        double x0 = a->origin[0] + x * C, y0 = a->origin[1] + y * C;
        world_to_canvas(x0, y0, z, us[0], vs[0]);
        world_to_canvas(x0 + C, y0, z, us[1], vs[1]);
        world_to_canvas(x0 + C, y0 + C, z, us[2], vs[2]);
        world_to_canvas(x0, y0 + C, z, us[3], vs[3]);
        if (drag_ == Drag::kShape) fill_poly(c, us, vs, 4, col, 110);
        for (int i = 0; i < 4; ++i) line_a(c, us[i], vs[i], us[(i + 1) % 4], vs[(i + 1) % 4], 0xFFFFFFFFu, 200);
    }
}

void EditMode::draw_dialog(RgbaCanvas& c) {
    if (!dialog_) return;
    const ShapeFont& f = fonts_.size() > 6 ? fonts_[6] : fonts_[0];
    const int u = ui_;
    Dialog& d = *dialog_;
    c.shade(0, 0, W_, H_, 0xFF000000u, 120);
    int dw = std::min(W_ - 20 * u, 300 * u), lines = d.confirm ? 1 : int(d.values.size()) * 2;
    int lh = (f.line_advance() + 4) * u;
    int dh = (lines + 4) * lh;
    int dx = (W_ - dw) / 2, dy = (H_ - dh) / 2;
    c.shade(dx, dy, dw, dh, 0xFF1C1E2Au, 245);
    rect_outline(c, dx, dy, dw, dh, 0xFFE0A040u);
    c.text(f, dx + 8 * u, dy + 5 * u, d.title, u);
    for (size_t i = 0; i < d.values.size(); ++i) {
        int fy = dy + (int(i) * 2 + 2) * lh;
        c.text(f, dx + 8 * u, fy - lh + 4 * u, d.labels[i], u);
        c.shade(dx + 8 * u, fy, dw - 16 * u, lh, 0xFF0A0B10u, 255);
        rect_outline(c, dx + 8 * u, fy, dw - 16 * u, lh, int(i) == d.focus ? 0xFFE0A040u : 0xFF50546Au);
        std::string s = d.values[i];
        bool caret = int(i) == d.focus && (ticks_ / 15) % 2 == 0;
        int maxw = (dw - 24 * u) / u;
        while (!s.empty() && f.text_width(s) > maxw) s.erase(0, 1);
        c.text(f, dx + 11 * u, fy + 2 * u, s + (caret ? "_" : ""), u);
    }
}

void EditMode::draw_help(RgbaCanvas& c) {
    if (!help || dialog_) return;
    const ShapeFont& f = fonts_.size() > 6 ? fonts_[6] : fonts_[0];
    const int u = ui_;
    static const char* lines[] = {
        "Edit mode: paint new areas on the world",
        "Left drag: use the tool.  Right / middle drag: pan.  Wheel or + -: zoom.  Arrows: pan.",
        "1-8: floor wall passage door water chasm stairs height.  B L R F E P M: brush line rect fill erase marker move",
        "[ ]: brush size, O: square / round.  , .: paint height.  PgUp / PgDn: layer (others ghosted).",
        "S: Segments (the default): drag any segment (a map or a painted area); Alt+drag with any tool.",
        "N: snap grid 512 / 256 / 128 / 64 / off (exact seam and teleporter offsets snap too; Shift: none).",
        "J: move joined pieces together.  Moving off a seam breaks it (a teleport); back on re-joins.",
        "Shift+click or a box (drag on empty ground; Shift+drag adds): select several segments.",
        "Layers panel: Reset to Pagan (the original placement) or to Baseline: the selection, or all.",
        "Original map contents are never edited (painted areas are separate): nothing to revert inside a segment.",
        "Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z): undo / redo (paint, areas, group moves and layers).  Ctrl+S: save.",
        "Tab: areas panel.  Y: layers panel (show / hide layers, every group and area; select, move, layer).",
        "Ctrl+arrows: nudge the selected group / area.  Ctrl+PgUp / PgDn: move it a layer up / down.",
        "Del: delete the area.  G: grid.  D: dress.",
        "Controller: left stick pointer, A paint, right stick pan, LT/RT zoom, LB/RB paint kind,",
        "  D-pad layer / brush size, X undo, Y redo, Back next tool, Start save, B back.",
        "Esc: back.  F1 or H: this help.",
    };
    int lh = (f.line_advance() + 3) * u;
    int n = int(sizeof lines / sizeof *lines);
    int w = 0;
    for (const char* l : lines) w = std::max(w, (f.text_width(l) + 12) * u);
    w = std::min(w, W_ - 8 * u);
    int h = n * lh + 10 * u, x = (W_ - w) / 2, y = std::max(top_h_ + 4 * u, (H_ - h) / 2);
    c.shade(x, y, w, h, 0xFF000000u, 215);
    rect_outline(c, x, y, w, h, 0xFFE0A040u);
    for (int i = 0; i < n; ++i) c.text(f, x + 6 * u, y + 5 * u + i * lh, lines[i], u);
}

// --- Layers and map groups (the Layers panel, Y) ---------------------------------------------

std::vector<int> EditMode::seam_cluster(int gi) const {
    std::vector<int> out;
    if (gi < 0 || size_t(gi) >= doc.layout.atlas.groups.size()) return out;
    nlohmann::json x = doc.layout.extra.empty() ? nlohmann::json::object() : nlohmann::json::parse(doc.layout.extra, nullptr, false);
    std::vector<std::pair<int, int>> edges;
    if (x.is_object() && x.contains("seams") && x["seams"].is_array())
        for (const auto& sm : x["seams"]) {
            if (!sm.is_object() || !sm.contains("maps") || !sm["maps"].is_array() || sm["maps"].size() != 2) continue;
            if (sm.value("mode", std::string("walk")) == "teleport") continue;  // broken: not joined
            int a = doc.layout.atlas.group_of(sm["maps"][0].get<unsigned>()), b = doc.layout.atlas.group_of(sm["maps"][1].get<unsigned>());
            if (a >= 0 && b >= 0 && a != b) edges.push_back({a, b});
        }
    std::set<int> seen = {gi};
    std::vector<int> todo = {gi};
    while (!todo.empty()) {
        int g = todo.back();
        todo.pop_back();
        out.push_back(g);
        for (auto [a, b] : edges) {
            int o = a == g ? b : b == g ? a : -1;
            if (o >= 0 && seen.insert(o).second) todo.push_back(o);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string EditMode::layer_name(int L) const {
    nlohmann::json x = doc.layout.extra.empty() ? nlohmann::json::object() : nlohmann::json::parse(doc.layout.extra, nullptr, false);
    std::string k = std::to_string(L);
    if (x.is_object() && x.contains("layers") && x["layers"].is_object() && x["layers"].contains(k) && x["layers"][k].is_string())
        return x["layers"][k].get<std::string>();
    return "";
}

std::vector<int> EditMode::layers_in_use() const {
    std::set<int> s = {layer, 0};
    for (const AtlasGroup& g : doc.layout.atlas.groups) s.insert(g.layer);
    for (const EditArea& a : doc.areas) s.insert(a.layer);
    return std::vector<int>(s.rbegin(), s.rend());  // top first
}

std::vector<std::string> EditMode::layout_problems() const {
    WorldLayoutFile f;
    std::string err;
    if (!f.from_json(doc.layout.to_json(), err)) return {"layout: " + err};
    std::vector<std::string> out;
    for (const std::string& p : check_world_layout(f, fps_))
        if (p.find("overlap") != std::string::npos || p.find("share z") != std::string::npos) out.push_back(p);
    return out;
}

void EditMode::select_group(int gi, bool centre) {
    if (gi < 0 || size_t(gi) >= doc.layout.atlas.groups.size()) return;
    selected_group = gi;
    selected.clear();
    const AtlasGroup& g = doc.layout.atlas.groups[size_t(gi)];
    layer = g.layer;
    hidden_layers.erase(layer);
    if (centre)
        for (unsigned m : g.maps)
            for (const MapFootprint& f : fps_)
                if (f.map == m && f.cx0 <= f.cx1) {
                    show_world_point(g.dx + (f.cx0 + f.cx1 + 1) * 256, g.dy + (f.cy0 + f.cy1 + 1) * 256, std::max(level_, 5));
                    return;
                }
}

void EditMode::after_group_change(const std::string& what) {
    refresh_exclusions();
    std::vector<std::string> probs = layout_problems();
    if (probs.empty()) say(what + " (no overlap on any layer)");
    else say(what + " - WARNING: " + probs[0] + (probs.size() > 1 ? " (+" + std::to_string(probs.size() - 1) + " more)" : ""), 360);
}

bool EditMode::nudge_selected(int dx, int dy) {
    if (EditArea* a = sel()) {
        doc.move_area(a->id, dx, dy, 0);
        say("Moved " + a->name + " by " + std::to_string(dx) + "," + std::to_string(dy));
        return true;
    }
    if (selected_group < 0 || size_t(selected_group) >= doc.layout.atlas.groups.size()) return false;
    std::vector<int> cl = moving_set(selected_group);
    doc.begin(cl.size() > 1 ? "move map group and its seam partners" : "move map group");
    for (int g : cl) doc.touch_group(g);
    doc.touch_layout_extra();
    for (int g : cl) {
        AtlasGroup& G = doc.layout.atlas.groups[size_t(g)];
        G.dx += dx;
        G.dy += dy;
        if (G.from_data || G.derived.find("moved by hand") == std::string::npos)
            G.derived = "manual: moved by hand in the editor (was: " + G.derived + ")";
        G.from_data = false;
    }
    std::string sm = refresh_seam_modes();
    doc.commit();
    const AtlasGroup& g = doc.layout.atlas.groups[size_t(selected_group)];
    after_group_change(g.label + (cl.size() > 1 ? " (+" + std::to_string(cl.size() - 1) + " seam partners)" : "") + " at " +
                       std::to_string(g.dx) + "," + std::to_string(g.dy) + sm);
    return true;
}

bool EditMode::selected_layer_step(int d) {
    if (EditArea* a = sel()) {
        doc.set_area_layer(a->id, a->layer + d);
        layer = a->layer;
        say(a->name + " on layer " + std::to_string(a->layer));
        return true;
    }
    if (selected_group < 0 || size_t(selected_group) >= doc.layout.atlas.groups.size()) return false;
    std::vector<int> cl = moving_set(selected_group);
    doc.begin("map group layer");
    for (int g : cl) doc.touch_group(g);
    doc.touch_layout_extra();
    for (int g : cl) {
        AtlasGroup& G = doc.layout.atlas.groups[size_t(g)];
        G.dz += d * kLayerZ;
        G.layer += d;
        if (G.derived.find("layer changed by hand") == std::string::npos)
            G.derived = "manual: layer changed by hand in the editor (was: " + G.derived + ")";
        G.from_data = false;
    }
    std::string sm = refresh_seam_modes();
    doc.commit();
    const AtlasGroup& g = doc.layout.atlas.groups[size_t(selected_group)];
    layer = g.layer;
    hidden_layers.erase(layer);
    after_group_change(g.label + " on layer " + std::to_string(g.layer) + sm);
    return true;
}

std::vector<int> EditMode::moving_set(int gi) const {
    if (move_joined) return seam_cluster(gi);
    return {gi};
}

std::vector<EditMode::SeamInfo> EditMode::seams() const {
    std::vector<SeamInfo> out;
    nlohmann::json x = doc.layout.extra.empty() ? nlohmann::json::object() : nlohmann::json::parse(doc.layout.extra, nullptr, false);
    if (!x.is_object() || !x.contains("seams") || !x["seams"].is_array()) return out;
    for (const auto& sm : x["seams"]) {
        if (!sm.is_object() || !sm.contains("maps") || !sm["maps"].is_array() || sm["maps"].size() != 2) continue;
        SeamInfo i{sm["maps"][0].get<unsigned>(), sm["maps"][1].get<unsigned>(), 0, 0, 0, sm.value("mode", std::string("walk")) != "teleport"};
        if (sm.contains("offset") && sm["offset"].is_array() && sm["offset"].size() >= 2) {
            i.dx = sm["offset"][0].get<int>();
            i.dy = sm["offset"][1].get<int>();
            if (sm["offset"].size() >= 3) i.dz = sm["offset"][2].get<int>();
        }
        out.push_back(i);
    }
    return out;
}

// Every seam whose two groups stand at its exact offset (same layer, same height difference)
// is walkable ("mode": "walk"); one moved off it is broken ("mode": "teleport": the original
// teleporters take the Avatar across). Call inside an open change after touch_layout_extra.
std::string EditMode::refresh_seam_modes() {
    nlohmann::json x = doc.layout.extra.empty() ? nlohmann::json::object() : nlohmann::json::parse(doc.layout.extra, nullptr, false);
    if (!x.is_object() || !x.contains("seams") || !x["seams"].is_array()) return "";
    std::string msg;
    bool changed = false;
    for (auto& sm : x["seams"]) {
        if (!sm.is_object() || !sm.contains("maps") || !sm["maps"].is_array() || sm["maps"].size() != 2) continue;
        unsigned a = sm["maps"][0].get<unsigned>(), b = sm["maps"][1].get<unsigned>();
        int ga = doc.layout.atlas.group_of(a), gb = doc.layout.atlas.group_of(b);
        if (ga < 0 || gb < 0 || !sm.contains("offset") || !sm["offset"].is_array() || sm["offset"].size() < 2) continue;
        const AtlasGroup &A = doc.layout.atlas.groups[size_t(ga)], &B = doc.layout.atlas.groups[size_t(gb)];
        int dz = sm["offset"].size() >= 3 ? sm["offset"][2].get<int>() : 0;
        bool exact = A.layer == B.layer && B.dx - A.dx == sm["offset"][0].get<int>() && B.dy - A.dy == sm["offset"][1].get<int>() &&
                     B.dz - A.dz == dz;
        bool walk = sm.value("mode", std::string("walk")) != "teleport";
        if (exact == walk) continue;
        sm["mode"] = exact ? "walk" : "teleport";
        changed = true;
        msg += std::string("; seam ") + std::to_string(a) + "-" + std::to_string(b) +
               (exact ? " re-joined (walkable again)" : " broken: its crossing is a teleport now");
    }
    if (changed) doc.layout.extra = x.dump();
    return msg;
}

// The offsets near (cx, cy) for the lead group where it lines up exactly with a group that is
// not moving: a seam's offset first, then any teleport (the source point onto its arrival point).
bool EditMode::snap_target(int lead, const std::vector<int>& moving, double cx, double cy, int& tx, int& ty, std::string& why) const {
    const AtlasGroup& L = doc.layout.atlas.groups[size_t(lead)];
    auto in_lead = [&](unsigned m) { return doc.layout.atlas.group_of(m) == lead; };
    auto other = [&](unsigned m) -> const AtlasGroup* {
        int g = doc.layout.atlas.group_of(m);
        if (g < 0 || std::find(moving.begin(), moving.end(), g) != moving.end()) return nullptr;
        return &doc.layout.atlas.groups[size_t(g)];
    };
    const double R = std::max(384, snap_grid);
    double best = R + 1;
    auto consider = [&](double x, double y, const std::string& w, double bias) {
        double d = std::hypot(x - cx, y - cy) - bias;
        if (d < best) { best = d; tx = int(x); ty = int(y); why = w; }
    };
    (void)L;
    for (const SeamInfo& s : seams()) {
        if (in_lead(s.a)) {
            if (const AtlasGroup* B = other(s.b)) consider(B->dx - s.dx, B->dy - s.dy, "the seam with map " + std::to_string(s.b), 64);
        } else if (in_lead(s.b)) {
            if (const AtlasGroup* A = other(s.a)) consider(A->dx + s.dx, A->dy + s.dy, "the seam with map " + std::to_string(s.a), 64);
        }
    }
    nlohmann::json x = doc.layout.extra.empty() ? nlohmann::json::object() : nlohmann::json::parse(doc.layout.extra, nullptr, false);
    if (x.is_object() && x.contains("links") && x["links"].is_array())
        for (const auto& l : x["links"]) {
            if (!l.is_object() || !l.contains("from") || !l.contains("to") || !l["from"].is_array() || !l["to"].is_array()) continue;
            unsigned fm = l.value("from_map", 0u), tm = l.value("to_map", 0u);
            int fx = l["from"][0].get<int>(), fy = l["from"][1].get<int>(), ttx = l["to"][0].get<int>(), tty = l["to"][1].get<int>();
            if (in_lead(fm)) {
                if (const AtlasGroup* T = other(tm)) consider(T->dx + ttx - fx, T->dy + tty - fy, "the teleporter to map " + std::to_string(tm), 0);
            } else if (in_lead(tm)) {
                if (const AtlasGroup* F = other(fm)) consider(F->dx + fx - ttx, F->dy + fy - tty, "the teleporter from map " + std::to_string(fm), 0);
            }
        }
    return best <= R;
}

void EditMode::start_group_drag(int gi) {
    selected_group = gi;
    selected.clear();
    drag_set_ = moving_set(gi);
    doc.begin(drag_set_.size() > 1 ? "move map group and its seam partners" : "move map group");
    for (int g : drag_set_) doc.touch_group(g);
    doc.touch_layout_extra();
    drag_start_[0] = doc.layout.atlas.groups[size_t(gi)].dx;
    drag_start_[1] = doc.layout.atlas.groups[size_t(gi)].dy;
    move_acc_[0] = move_acc_[1] = 0;
    moved_[0] = moved_[1] = 0;
    snap_why_.clear();
    drag_ = Drag::kMoveGroup;
}

void EditMode::group_drag_update() {
    if (selected_group < 0 || drag_set_.empty()) return;
    double cx = drag_start_[0] + move_acc_[0], cy = drag_start_[1] + move_acc_[1];
    int tx, ty;
    std::string why;
    if (snap_grid > 0) {
        tx = int(std::lround(cx / snap_grid)) * snap_grid;
        ty = int(std::lround(cy / snap_grid)) * snap_grid;
    } else {
        tx = int(std::lround(cx));
        ty = int(std::lround(cy));
    }
    snap_why_.clear();
    int sx, sy;
    if (!shift_ && snap_target(selected_group, drag_set_, cx, cy, sx, sy, why)) { tx = sx; ty = sy; snap_why_ = why; }
    AtlasGroup& lead = doc.layout.atlas.groups[size_t(selected_group)];
    int ddx = tx - lead.dx, ddy = ty - lead.dy;
    if (!ddx && !ddy) return;
    for (int g : drag_set_) {
        doc.layout.atlas.groups[size_t(g)].dx += ddx;
        doc.layout.atlas.groups[size_t(g)].dy += ddy;
    }
    moved_[0] = tx - drag_start_[0];
    moved_[1] = ty - drag_start_[1];
}

void EditMode::finish_group_drag() {
    if (selected_group < 0) { doc.commit(); drag_set_.clear(); return; }
    std::string sm;
    if (moved_[0] || moved_[1]) {
        for (int gi : drag_set_) {
            AtlasGroup& g = doc.layout.atlas.groups[size_t(gi)];
            if (g.from_data || g.derived.find("moved by hand") == std::string::npos)
                g.derived = "manual: moved by hand in the editor (was: " + g.derived + ")";
            g.from_data = false;
        }
        sm = refresh_seam_modes();
    }
    doc.commit();
    const AtlasGroup& g = doc.layout.atlas.groups[size_t(selected_group)];
    size_t n = drag_set_.size();
    drag_set_.clear();
    if (moved_[0] || moved_[1])
        after_group_change(g.label + (n > 1 ? " (+" + std::to_string(n - 1) + " joined)" : "") + " moved to " + std::to_string(g.dx) +
                           "," + std::to_string(g.dy) + (snap_why_.empty() ? "" : ", lined up with " + snap_why_) + sm);
    else
        say(g.label + " selected (drag to move; Layers panel: Y)");
}

std::string EditMode::drag_group_by(int gi, double dx, double dy) {
    start_group_drag(gi);
    move_acc_[0] = dx;
    move_acc_[1] = dy;
    group_drag_update();
    drag_ = Drag::kNone;
    finish_group_drag();
    return message;
}

void EditMode::set_reference_layout(int which, const std::string& t) {
    if (which == kPagan || which == kBaseline) ref_[which] = t;
}

bool EditMode::has_reference(int which) const { return (which == kPagan || which == kBaseline) && !ref_[which].empty(); }

std::vector<int> EditMode::selection() const {
    std::vector<int> out;
    if (selected_group >= 0 && size_t(selected_group) < doc.layout.atlas.groups.size()) out.push_back(selected_group);
    for (int g : multi)
        if (g != selected_group && g >= 0 && size_t(g) < doc.layout.atlas.groups.size()) out.push_back(g);
    return out;
}

void EditMode::shown_place(int gi, int& dx, int& dy, int& lay) const {
    const AtlasGroup& g = doc.layout.atlas.groups[size_t(gi)];
    dx = g.dx; dy = g.dy; lay = g.layer;
    if (!show_spots || !has_reference(show_spots)) return;
    // (parsed per call: views are rare and the layouts small)
    static std::string cached_text;
    static AtlasLayout cached;
    if (cached_text != ref_[show_spots]) {
        std::string err;
        cached = AtlasLayout{};
        cached.from_json(ref_[show_spots], err);
        cached_text = ref_[show_spots];
    }
    int r = cached.group_of(g.maps[0]);
    if (r < 0) return;
    dx = cached.groups[size_t(r)].dx;
    dy = cached.groups[size_t(r)].dy;
    if (show_spots == kBaseline) lay = cached.groups[size_t(r)].layer;
}

int EditMode::select_box(double u0, double v0, double u1, double v1, bool add) {
    if (u0 > u1) std::swap(u0, u1);
    if (v0 > v1) std::swap(v0, v1);
    if (!add) { selected_group = -1; multi.clear(); }
    int n = 0;
    for (size_t gi = 0; gi < doc.layout.atlas.groups.size(); ++gi) {
        const AtlasGroup& g = doc.layout.atlas.groups[gi];
        if (g.layer != layer || hidden_layers.count(g.layer)) continue;
        bool hit = false;
        for (unsigned m : g.maps)
            for (const MapFootprint& f : fps_) {
                if (f.map != m || hit) continue;
                for (int cy = f.cy0; cy <= f.cy1 && !hit; ++cy)
                    for (int cx = f.cx0; cx <= f.cx1 && !hit; ++cx) {
                        if (!f.has(cx, cy)) continue;
                        // The chunk's diamond on screen (ground level) against the box.
                        double us[4], vs[4];
                        world_to_canvas(g.dx + cx * 512, g.dy + cy * 512, f.median_z, us[0], vs[0]);
                        world_to_canvas(g.dx + cx * 512 + 512, g.dy + cy * 512, f.median_z, us[1], vs[1]);
                        world_to_canvas(g.dx + cx * 512 + 512, g.dy + cy * 512 + 512, f.median_z, us[2], vs[2]);
                        world_to_canvas(g.dx + cx * 512, g.dy + cy * 512 + 512, f.median_z, us[3], vs[3]);
                        double a = std::min({us[0], us[1], us[2], us[3]}), b = std::max({us[0], us[1], us[2], us[3]});
                        double c = std::min({vs[0], vs[1], vs[2], vs[3]}), e = std::max({vs[0], vs[1], vs[2], vs[3]});
                        hit = b >= u0 && a <= u1 && e >= v0 && c <= v1;
                    }
            }
        if (!hit) continue;
        ++n;
        if (selected_group < 0) selected_group = int(gi);
        else if (int(gi) != selected_group) multi.insert(int(gi));
    }
    selected.clear();
    return n;
}

bool EditMode::reset_group(int gi, int which) {
    if (gi < 0 || size_t(gi) >= doc.layout.atlas.groups.size() || !has_reference(which)) return false;
    AtlasLayout ref;
    std::string err;
    if (!ref.from_json(ref_[which], err)) return false;
    AtlasGroup& g = doc.layout.atlas.groups[size_t(gi)];
    int r = ref.group_of(g.maps[0]);
    if (r < 0) { say(g.label + ": not in " + reference_name(which)); return false; }
    const AtlasGroup& R = ref.groups[size_t(r)];
    const bool outer = !doc.in_change();
    if (outer) doc.begin(std::string("reset ") + g.label + " to " + reference_name(which));
    doc.touch_group(gi);
    doc.touch_layout_extra();
    g.dx = R.dx;
    g.dy = R.dy;
    if (which == kBaseline) { g.dz = R.dz; g.layer = R.layer; }  // Pagan (the atlas) has no layers: the group keeps its own
    g.derived = which == kBaseline ? R.derived : std::string("reset in the editor to its Pagan spot (") + R.derived + ")";
    g.from_data = R.from_data;
    if (outer) {
        std::string sm = refresh_seam_modes();
        doc.commit();
        layer = g.layer;
        after_group_change(g.label + " back at its " + reference_name(which) + " spot" + sm);
    }
    return true;
}

bool EditMode::reset_selection(int which) {
    std::vector<int> sel_ = selection();
    if (sel_.empty() || !has_reference(which)) { say("Select segments first (click; Shift+click or a box for more)"); return false; }
    if (sel_.size() == 1) return reset_group(sel_[0], which);
    doc.begin(std::string("reset ") + std::to_string(sel_.size()) + " segments to " + reference_name(which));
    int n = 0;
    for (int gi : sel_) n += reset_group(gi, which);
    std::string sm = refresh_seam_modes();
    doc.commit();
    after_group_change(std::to_string(n) + " segments back at their " + reference_name(which) + " spots" + sm);
    return n > 0;
}

bool EditMode::reset_all(int which) {
    if (!has_reference(which)) return false;
    doc.begin(std::string("reset all to ") + reference_name(which));
    int n = 0;
    for (size_t gi = 0; gi < doc.layout.atlas.groups.size(); ++gi) n += reset_group(int(gi), which);
    if (which == kBaseline) {
        // The Baseline's seams, links, variants and layer names too (its whole linking state).
        nlohmann::json b = nlohmann::json::parse(ref_[which], nullptr, false, true);
        nlohmann::json x = doc.layout.extra.empty() ? nlohmann::json::object() : nlohmann::json::parse(doc.layout.extra, nullptr, false);
        if (b.is_object() && x.is_object()) {
            for (const char* k : {"seams", "links", "variants", "layers", "layout_revision"})
                if (b.contains(k)) x[k] = b[k];
            doc.touch_layout_extra();
            doc.layout.extra = x.dump();
        }
    }
    std::string sm = refresh_seam_modes();
    doc.commit();
    after_group_change(std::to_string(n) + " segments back at their " + reference_name(which) + " spots" + sm);
    return n > 0;
}

const std::set<int>& EditMode::overlapping_groups() {
    if (overlap_stamp_ == doc.changes) return overlap_;
    overlap_stamp_ = doc.changes;
    overlap_.clear();
    for (const std::string& p : layout_problems()) {
        // "groups A and B overlap on layer L ..." / "groups A (layer ...) and B (...) share z ..."
        if (p.rfind("groups ", 0) != 0) continue;
        std::string rest = p.substr(7);
        size_t sp = rest.find(' ');
        size_t an = rest.find(" and ");
        if (sp == std::string::npos || an == std::string::npos) continue;
        std::string a = rest.substr(0, sp), b = rest.substr(an + 5);
        b = b.substr(0, b.find(' '));
        for (size_t gi = 0; gi < doc.layout.atlas.groups.size(); ++gi)
            if (doc.layout.atlas.groups[gi].name == a || doc.layout.atlas.groups[gi].name == b) overlap_.insert(int(gi));
    }
    return overlap_;
}

// The segment snap grid (world lines every snap_grid units) on the current layer, in the
// Segments tool when the grid is on and the lines are at least 8 pixels apart.
void EditMode::draw_segment_grid(RgbaCanvas& c) {
    if (!show_grid || snap_grid <= 0 || (tool != Tool::kSegments && tool != Tool::kMove)) return;
    const double k = std::ldexp(1.0, -level_);
    if (snap_grid * k / 2 < 8) return;
    double xs[4], ys[4];
    canvas_to_world(0, 0, 0, xs[0], ys[0]);
    canvas_to_world(W_, 0, 0, xs[1], ys[1]);
    canvas_to_world(W_, H_, 0, xs[2], ys[2]);
    canvas_to_world(0, H_, 0, xs[3], ys[3]);
    double x0 = *std::min_element(xs, xs + 4), x1 = *std::max_element(xs, xs + 4);
    double y0 = *std::min_element(ys, ys + 4), y1 = *std::max_element(ys, ys + 4);
    long i0 = long(std::floor(x0 / snap_grid)), i1 = long(std::ceil(x1 / snap_grid));
    long j0 = long(std::floor(y0 / snap_grid)), j1 = long(std::ceil(y1 / snap_grid));
    if ((i1 - i0) + (j1 - j0) > 600) return;
    for (long i = i0; i <= i1; ++i) {
        double ua, va, ub, vb;
        world_to_canvas(double(i) * snap_grid, y0, 0, ua, va);
        world_to_canvas(double(i) * snap_grid, y1, 0, ub, vb);
        line_a(c, ua, va, ub, vb, 0xFF8080A0u, i % 4 ? 45 : 90);
    }
    for (long j = j0; j <= j1; ++j) {
        double ua, va, ub, vb;
        world_to_canvas(x0, double(j) * snap_grid, 0, ua, va);
        world_to_canvas(x1, double(j) * snap_grid, 0, ub, vb);
        line_a(c, ua, va, ub, vb, 0xFF8080A0u, j % 4 ? 45 : 90);
    }
}

// Outlines of the map groups: this layer's in white (the selected one yellow) with its name;
// the other layers' as ghosts (blue below, green above) with an "above +N" / "below -N" badge.
void EditMode::draw_group_outlines(RgbaCanvas& c) {
    const ShapeFont& f = fonts_.size() > 6 ? fonts_[6] : fonts_[0];
    const double k = std::ldexp(1.0, -level_);
    draw_segment_grid(c);
    const std::set<int>& over = overlapping_groups();
    for (size_t gi = 0; gi < doc.layout.atlas.groups.size(); ++gi) {
        AtlasGroup g = doc.layout.atlas.groups[gi];
        shown_place(int(gi), g.dx, g.dy, g.layer);
        if (hidden_layers.count(g.layer)) continue;
        const bool hov = int(gi) == hover_group_ && drag_ == Drag::kNone;
        const bool ov = over.count(int(gi)) > 0;
        int d = g.layer - layer;
        int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
        for (unsigned m : g.maps)
            for (const MapFootprint& fp : fps_)
                if (fp.map == m && fp.cx0 <= fp.cx1) {
                    x0 = std::min(x0, fp.cx0 * 512); y0 = std::min(y0, fp.cy0 * 512);
                    x1 = std::max(x1, (fp.cx1 + 1) * 512); y1 = std::max(y1, (fp.cy1 + 1) * 512);
                }
        if (x0 > x1) continue;
        double us[4], vs[4];
        world_to_canvas(x0 + g.dx, y0 + g.dy, 0, us[0], vs[0]);
        world_to_canvas(x1 + g.dx, y0 + g.dy, 0, us[1], vs[1]);
        world_to_canvas(x1 + g.dx, y1 + g.dy, 0, us[2], vs[2]);
        world_to_canvas(x0 + g.dx, y1 + g.dy, 0, us[3], vs[3]);
        if (us[1] < -50 || us[3] > W_ + 50 || vs[0] > H_ + 50 || vs[2] < -50) continue;
        const bool sel_g = int(gi) == selected_group || multi.count(int(gi)) > 0;
        bool unplaced = g.extra.find("\"unplaced\":true") != std::string::npos;
        uint32_t col = sel_g ? 0xFFFFE060u : hov && d == 0 ? 0xFF60FFFFu : ov && d == 0 ? 0xFFFF4040u
                     : d == 0 ? (unplaced ? 0xFFFF9050u : 0xFFE8E8F0u) : d < 0 ? 0xFF60A0FFu : 0xFF80FFB0u;
        int alpha = sel_g || hov ? 255 : ov && d == 0 ? 230 : d == 0 ? 120 : 150;
        for (int i = 0; i < 4; ++i) {
            if (d == 0 || sel_g) {
                line_a(c, us[i], vs[i], us[(i + 1) % 4], vs[(i + 1) % 4], col, alpha);
                if (sel_g || hov || ov) line_a(c, us[i], vs[i] + 1, us[(i + 1) % 4], vs[(i + 1) % 4] + 1, col, alpha);
            } else {  // dashed: a ghost of another layer
                double ax = us[i], ay = vs[i], bx = us[(i + 1) % 4], by = vs[(i + 1) % 4];
                double len = std::hypot(bx - ax, by - ay);
                int n = std::max(1, int(len / (6.0 * ui_)));
                for (int j = 0; j < n; j += 2)
                    line_a(c, ax + (bx - ax) * j / n, ay + (by - ay) * j / n, ax + (bx - ax) * (j + 1) / n, ay + (by - ay) * (j + 1) / n, col, alpha);
            }
        }
        if ((level_ > 8 || std::abs(d) > 1 || (d != 0 && level_ > 7)) && !sel_g && !hov) continue;  // labels: this layer and the next ones
        std::string lab = g.label;
        if (ov && d == 0) lab = "OVERLAP: " + lab;
        if (hov && d == 0) lab = "Drag to move: " + lab;
        if (d > 0) lab = "ABOVE (layer " + std::to_string(g.layer) + "): " + lab;
        else if (d < 0) lab = "BELOW (layer " + std::to_string(g.layer) + "): " + lab;
        else if (unplaced && lab.find("(unplaced)") == std::string::npos) lab = "UNPLACED: " + lab;
        if (lab.size() > 46) lab = lab.substr(0, 44) + "..";
        int tw = (f.text_width(lab) + 6) * ui_;
        double cu = (us[0] + us[2]) / 2, cv = (vs[1] + vs[3]) / 2 + (d > 0 ? -10 : d < 0 ? 10 : 0) * ui_ * std::min(1.0, k * 64);
        int lu = int(cu) - tw / 2, lv = int(cv) - (f.line_advance() + 3) * ui_ / 2;
        c.shade(lu, lv, tw, (f.line_advance() + 3) * ui_, d == 0 ? 0xFF000000u : d < 0 ? 0xFF102040u : 0xFF103020u, d == 0 ? 150 : 190);
        c.text(f, lu + 3 * ui_, lv + ui_, lab, ui_);
    }
}

void draw_box_(RgbaCanvas& c, const double* b) {
    double u0 = std::min(b[0], b[2]), u1 = std::max(b[0], b[2]), v0 = std::min(b[1], b[3]), v1 = std::max(b[1], b[3]);
    c.shade(int(u0), int(v0), int(u1 - u0), int(v1 - v0), 0xFF60FFFFu, 40);
    line_a(c, u0, v0, u1, v0, 0xFF60FFFFu, 255);
    line_a(c, u1, v0, u1, v1, 0xFF60FFFFu, 255);
    line_a(c, u1, v1, u0, v1, 0xFF60FFFFu, 255);
    line_a(c, u0, v1, u0, v0, 0xFF60FFFFu, 255);
}

// The current layer, clearly: a strip under the toolbar ("Layer 0 - the surface ...").
void EditMode::draw_layer_banner(RgbaCanvas& c) {
    if (dialog_) return;
    const ShapeFont& f = fonts_.size() > 6 ? fonts_[6] : fonts_[0];
    std::string nm = layer_name(layer);
    std::string t = "LAYER " + std::to_string(layer) + (layer == 0 ? " (ground)" : layer < 0 ? " (below ground)" : " (above)") +
                    (nm.empty() ? "" : " - " + nm);
    int maxw = (panel || layers_panel ? panel_x_ : W_) - 12 * ui_;
    while (t.size() > 8 && (f.text_width(t) + 8) * ui_ > maxw) t = t.substr(0, t.size() - 4) + "..";
    int tw = (f.text_width(t) + 8) * ui_, th = (f.line_advance() + 4) * ui_;
    uint32_t bg = layer == 0 ? 0xFF305020u : layer < 0 ? 0xFF203060u : 0xFF503050u;
    c.shade(4 * ui_, top_h_ + 3 * ui_, tw, th, bg, 230);
    c.text(f, 8 * ui_, top_h_ + 5 * ui_, t, ui_);
}

void EditMode::render(std::vector<uint32_t>& out, int& w, int& h, int S) {
    S_ = std::max(2, S);
    W_ = 320 * S_;
    H_ = 200 * S_;
    ui_ = std::max(1, S_ / 2);
    out.resize(size_t(W_) * H_);
    w = W_;
    h = H_;
    if (!loaded_ && !load_all()) {
        std::fill(out.begin(), out.end(), kBackground);
        RgbaCanvas c{out.data(), W_, H_, &data_.palette};
        if (!fonts_.empty()) c.text(fonts_.size() > 6 ? fonts_[6] : fonts_[0], 10, 10, "Opening the browser storage...", ui_);
        return;
    }
    draw_maps(out);
    RgbaCanvas c{out.data(), W_, H_, &data_.palette};
    draw_grid(c);
    if (dressed_ && dressed_->area != selected) dressed_.reset();
    draw_dressed(c);
    draw_group_outlines(c);
    if (drag_ == Drag::kBox) draw_box_(c, box_);
    draw_areas(c, out.data());
    draw_preview(c);
    layout_ui();
    draw_ui(c);
    draw_layer_banner(c);
    draw_dress_report(c);
    draw_dialog(c);
    draw_help(c);
    if (const Shape* m = data_.mouse.shape(0))
        if (const ShapeFrame* fr = m->frame(drag_ == Drag::kMoveArea || drag_ == Drag::kMoveGroup || drag_ == Drag::kPan ? 36 : 34))
            c.frame(*fr, int(mouse_u_), int(mouse_v_), ui_);
}

void EditMode::publish_state() {
#ifdef __EMSCRIPTEN__
    std::string dress_text;
    for (const std::string& l : dress_report()) dress_text += l + "\n";
    size_t cells = 0, pois = 0;
    std::string names;
    for (const EditArea& a : doc.areas) {
        cells += a.cells.size();
        pois += a.pois.size();
        names += (names.empty() ? "" : "\n") + a.name;
    }
    EM_ASM({
        try {
            Module.pgEditState = ({areas: $0, cells: $1, pois: $2, names: UTF8ToString($3), saves: $4, layoutRev: $5,
                                   dressedItems: $6, dressOk: $7, dressReport: UTF8ToString($8)});
        } catch (e) {}
    }, int(doc.areas.size()), int(cells), int(pois), names.c_str(), save_gen_, layout_rev_,
       int(dressed_ ? dress_items_() : 0), int(dressed_ && dress_ok_()), dress_text.c_str());
#endif
}

void setup_editor_storage(EditMode& m, const std::string& data_dir) {
    // The reference placements for Reset (shipped with the program; never written by the editor).
    auto slurp = [](const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    m.set_reference_layout(EditMode::kPagan, slurp(data_dir + "/atlas_layout.json"));
    m.set_reference_layout(EditMode::kBaseline, slurp(data_dir + "/world_layout.baseline.json"));
#ifdef __EMSCRIPTEN__
    EM_ASM({
        try {
            try { FS.mkdir('/editor'); } catch (e) {}
            FS.mount(IDBFS, {}, '/editor');
            Module.pgEditReady = 0;
            FS.syncfs(true, function(err) {
                Module.pgEditReady = err ? -1 : 1;
                if (err) console.warn('editor: IndexedDB read failed', err);
                else console.log('editor: browser storage ready');
            });
        } catch (e) {
            console.warn('editor: no browser storage', e);
            Module.pgEditReady = -1;
        }
        // Writes go to IndexedDB shortly after the last one.
        Module.pgEditSync = function() {
            try {
                if (Module.pgEditSyncT) clearTimeout(Module.pgEditSyncT);
                Module.pgEditSyncT = setTimeout(function() {
                    Module.pgEditSyncT = 0;
                    try {
                        FS.syncfs(false, function(err) {
                            if (err) console.warn('editor: IndexedDB write failed', err);
                            else Module.pgEditSynced = (Module.pgEditSynced || 0) + 1;
                        });
                    } catch (e) { console.warn('editor: IndexedDB write failed', e); }
                }, 300);
            } catch (e) {}
        };
    });
    auto fs_ready = []() { return EM_ASM_INT({ try { return Module.pgEditReady ? 1 : 0; } catch (e) { return 1; } }) != 0; };
    auto fs_sync = []() { EM_ASM({ try { if (Module.pgEditSync) Module.pgEditSync(); } catch (e) {} }); };
    {
        // In this browser only (Download / Upload in the editor make copies).
        auto st = std::make_unique<FileStorage>("/editor/areas", "/editor/world_layout.json", "/editor/history");
        st->ready_check = fs_ready;
        st->on_write = fs_sync;
        m.set_storage(std::move(st));
    }
    m.set_seed_storage(std::make_unique<FileStorage>(data_dir + "/areas", data_dir + "/world_layout.json", ""));
    m.set_autosave(true);
#else
    const char* home = std::getenv("HOME");
    std::string hist = home ? std::string(home) + "/.pagan/editor_history" : data_dir + "/../build/editor_history";
    m.set_storage(std::make_unique<FileStorage>(data_dir + "/areas", data_dir + "/world_layout.json", hist));
#endif
}

}  // namespace pg
