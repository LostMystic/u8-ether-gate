// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The gump (window) system and every in-game gump: containers, the paperdoll, mini-stats,
// the quantity slider, readers, the quit box and the diary book (main menu, name entry,
// save / load, options). specs/presentation/gumps.md, text.md, input.md.
#include "game/gumps.h"

#include <algorithm>
#include <cstdio>

#include "core/config.h"
#include "core/log.h"
#include "core/serial.h"
#include "data/gamedata.h"
#include "game/combat.h"
#include "game/container_sort.h"
#include "game/credits.h"
#include "game/movie.h"
#include "game/leveling.h"
#include "game/party.h"
#include "game/processes.h"
#include "game/savegame.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/input.h"

namespace pg {

// ---------------------------------------------------------------------------------------------
// Gump

const ShapeFrame* Gump::gump_frame(unsigned gshape, unsigned gframe) const {
    return w_.data().gumps.frame(gshape, gframe);
}

void Gump::draw_shape(Surface8& s, unsigned gshape, unsigned gframe, int lx, int ly) const {
    if (const ShapeFrame* f = gump_frame(gshape, gframe)) s.draw(*f, x + lx + f->xoff, y + ly + f->yoff);
}

void Gump::paint(Surface8& s) {
    if (shape) draw_shape(s, shape, frame, 0, 0);
}

bool Gump::hit(int lx, int ly) const {
    if (modal) return true;
    if (!shape) return lx >= 0 && ly >= 0 && lx < w && ly < h;
    const ShapeFrame* f = gump_frame(shape, frame);
    return f && f->opaque(lx, ly);
}

void Gump::save(SaveWriter& out) const {
    out.i32(x); out.i32(y); out.u16(owner);
}

void Gump::load(SaveReader& in) {
    x = in.i32(); y = in.i32(); owner = uint16_t(in.u16());
}

// ---------------------------------------------------------------------------------------------
// Gumps (the desktop)

Gump* Gumps::add(std::unique_ptr<Gump> g) {
    auto pos = list.end();
    while (pos != list.begin() && (*(pos - 1))->layer > g->layer) --pos;
    if (g->pauses) w_.kernel().pause();
    g->open_seq = ++seq_;
    Gump* raw = g.get();
    list.insert(pos, std::move(g));
    return raw;
}

Gump* Gumps::newest_window() const {
    Gump* best = nullptr;
    for (const auto& g : list)
        if (!g->closed && !g->modal && std::string(g->kind()) != "partybar" && std::string(g->kind()) != "levelprompt" && (!best || g->open_seq > best->open_seq))
            best = g.get();
    return best;
}

void Gumps::raise(Gump* g) {
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].get() == g) {
            std::unique_ptr<Gump> keep = std::move(list[i]);
            list.erase(list.begin() + long(i));
            auto pos = list.end();
            while (pos != list.begin() && (*(pos - 1))->layer > keep->layer) --pos;
            list.insert(pos, std::move(keep));
            return;
        }
}

bool Gumps::is_open(const Gump* g) const {
    for (auto& p : list)
        if (p.get() == g) return !p->closed;
    return false;
}

void Gumps::close_all() {
    for (auto& g : list) g->close();
    reap();
}

void Gumps::close_item_dependent() {
    for (auto& g : list)
        if (g->item_dependent) g->close();
    reap();
}

void Gumps::reap() {
    for (int guard = 0; guard < 8; ++guard) {
        std::vector<std::unique_ptr<Gump>> dead;
        for (size_t i = 0; i < list.size();) {
            if (list[i]->closed) {
                dead.push_back(std::move(list[i]));
                list.erase(list.begin() + long(i));
            } else {
                ++i;
            }
        }
        if (dead.empty()) return;
        for (auto& g : dead) {
            g->on_close();
            if (g->pauses) w_.kernel().unpause();
            if (Process* p = w_.kernel().get(g->notify_pid); p && p->live()) {
                p->result = g->result;
                p->terminate();
            }
        }
    }
}

void Gumps::tick() {
    for (size_t i = 0; i < list.size(); ++i) {
        Gump* g = list[i].get();
        if (!g->closed) g->tick();
    }
    reap();
}

void Gumps::paint(Surface8& s, int min_layer, int max_layer) {
    for (auto& g : list)
        if (!g->closed && g->layer >= min_layer && g->layer <= max_layer) {
            s.set_clip(0, 0, s.width(), s.height());
            g->paint(s);
        }
    s.reset_clip();
}

Gump* Gumps::at(int sx, int sy) const {
    for (size_t i = list.size(); i-- > 0;) {
        Gump* g = list[i].get();
        if (g->closed) continue;
        if (g->modal) return g;
        if (g->contains(sx, sy) && g->hit(sx - g->x, sy - g->y)) return g;
    }
    return nullptr;
}

Gump* Gumps::top_modal() const {
    for (size_t i = list.size(); i-- > 0;)
        if (!list[i]->closed && list[i]->modal) return list[i].get();
    return nullptr;
}

Gump* Gumps::container_gump(uint16_t item) const {
    for (auto& g : list)
        if (!g->closed && g->owner == item && dynamic_cast<ContainerGump*>(g.get())) return g.get();
    return nullptr;
}

// Only the item-dependent windows are saved: modal gumps block saving (gumps.md 1.3).
void Gumps::save(SaveWriter& out) const {
    std::vector<const Gump*> keep;
    for (auto& g : list)
        if (!g->closed && !g->modal && (dynamic_cast<const ContainerGump*>(g.get()) || dynamic_cast<const MiniStatsGump*>(g.get())))
            keep.push_back(g.get());
    out.u16(uint32_t(keep.size()));
    for (const Gump* g : keep) {
        out.str(g->kind());
        out.u16(g->shape);
        g->save(out);
    }
}

bool Gumps::load(SaveReader& in) {
    list.clear();
    unsigned n = in.u16();
    for (unsigned i = 0; i < n && !in.fail(); ++i) {
        std::string k = in.str();
        unsigned sh = in.u16();
        std::unique_ptr<Gump> g;
        if (k == "container") g = std::make_unique<ContainerGump>(w_, 0, sh);
        else if (k == "paperdoll") g = std::make_unique<PaperdollGump>(w_, 1);
        else if (k == "ministats") g = std::make_unique<MiniStatsGump>(w_);
        else { in.set_fail(); return false; }
        g->load(in);
        add(std::move(g));
    }
    return !in.fail();
}

// ---------------------------------------------------------------------------------------------
// Containers (gumps.md 2.6)

namespace {
const std::vector<uint8_t>& ghost_table(const World& w) {
    static std::vector<uint8_t> t;
    if (t.empty()) t = build_ghost_table(w.data().palette);
    return t;
}
}  // namespace

ContainerGump::ContainerGump(World& w, uint16_t item, unsigned gump_shape) : Gump(w) {
    owner = item;
    shape = gump_shape;
    draggable = true;
    item_dependent = true;
    if (const ShapeFrame* f = gump_frame(shape, 0)) { this->w = f->w; h = f->h; }
    else { this->w = 100; h = 60; }
    GameData::GumpArea a = w.data().gump_area(shape);
    if (a.empty()) a = {8, 8, this->w - 8, h - 8};
    area_l = a.left; area_t = a.top; area_w = a.right - a.left; area_h = a.bottom - a.top;
}

void ContainerGump::follow_item() {
    const Item* it = w_.item(owner);
    if (!it) return;
    int ax, ay;
    const Item* root = w_.root_container(*it);
    w_.screen_point(*root, ax, ay);
    ay -= int(w_.data().typeflags.get(root->shape).z()) * 8 + 16;
    int nx = ax - w / 2, ny = ay - h;
    if (!placed) {
        // Clamped once, when it opens (gumps.md 1.4).
        int cx = std::clamp(nx, 0, std::max(0, 320 - w)), cy = std::clamp(ny, 0, std::max(0, 200 - h));
        off_x = cx - nx;
        off_y = cy - ny;
        placed = true;
    }
    x = nx + off_x;
    y = ny + off_y;
}

void ContainerGump::place_contents() {
    Item* c = w_.item(owner);
    if (!c) return;
    for (uint16_t k : c->contents) {
        Item* it = w_.item(k);
        if (!it || (it->flags & kItemEquipped)) continue;
        const ShapeFrame* f = w_.data().shapes.frame(it->shape, it->frame);
        if (!f) continue;
        int gx = it->y & 0xFF, gy = (it->y >> 8) & 0xFF;
        // The whole drawn frame inside the area (a mirrored frame extends the other way).
        int lo = (it->flags & kItemFlipped) ? f->w - 1 - f->xoff : f->xoff;
        int x0 = lo, x1 = area_w + lo - f->w, y0 = f->yoff, y1 = area_h + f->yoff - f->h;
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
        if ((it->y & 0xFFFF) == 0xFFFF) {
            gx = x0 + w_.rand_below(x1 - x0 + 1);
            gy = y0 + w_.rand_below(y1 - y0 + 1);
        }
        gx = std::clamp(gx, x0, x1);
        gy = std::clamp(gy, y0, y1);
        it->y = int32_t((std::clamp(gy, 0, 254) << 8) | std::clamp(gx, 0, 254));
    }
}

void ContainerGump::tick() {
    const Item* it = w_.item(owner);
    if (!it || (it->as_actor() == nullptr && !w_.is_container(*it))) { close(); return; }
    follow_item();
    place_contents();
    // After a sort: lay out again once the combine events have run (a pile frame may change).
    if (sort_wait > 0) {
        bool running = false;
        for (uint16_t pid : sort_pids)
            if (Process* p = w_.kernel().get(pid); p && p->live()) running = true;
        if (!running || --sort_wait == 0) {
            sort_wait = 0;
            sort_pids.clear();
            sort_relayout(w_, *this);
        }
    }
}

// The Sort button: right-aligned under the item area (above it if there is no room).
bool ContainerGump::sort_button(int& bx, int& by, int& bw, int& bh) const {
    if (!can_sort()) return false;
    const ShapeFont& f = w_.font(4);
    bw = (f.valid() ? f.text_width("SORT") : 20) + 9;
    bh = 11;
    bx = std::max(0, area_l + area_w - bw);
    by = area_t + area_h + 2;
    if (by + bh > h) by = area_t - bh - 2;
    if (by < 0) by = 0;
    return true;
}

namespace {
// The button is built from the slider's OK button (U8GUMPS 42): its left border, a plain
// middle column stretched, its right border, and the label in the save-screen font (4).
void paint_sort_button(Surface8& s, const World& w, const ShapeFrame* ok, int x, int y, int bw, bool down,
                       const char* label = "SORT") {
    if (ok && ok->w >= 8 && ok->h == 11) {
        auto col = [&](int src, int dst) {
            for (int r = 0; r < ok->h; ++r) {
                int sy = y + r, sx = x + dst;
                uint8_t c = ok->pixels[size_t(r) * ok->w + src];
                if (c != ShapeFrame::kTransparent && sx >= 0 && sy >= 0 && sx < s.width() && sy < s.height()) s.row(sy)[sx] = c;
            }
        };
        for (int c = 0; c < bw; ++c) col(c < 3 ? c : c >= bw - 2 ? ok->w - (bw - c) : 3, c);
    } else {
        s.fill_rect(x, y, bw, 11, 0xE6);
    }
    const ShapeFont& f = w.font(4);
    if (!f.valid()) return;
    int tw = f.text_width(label);
    f.draw_top(s, x + (bw - tw) / 2 + (down ? 1 : 0), y + (11 - f.height()) / 2 + (down ? 1 : 0), label);
}
}  // namespace

bool ContainerGump::foreign() const {
    const Item* c = w_.item(owner);
    if (!c || owner == 1) return false;
    const Item* r = w_.root_container(*c);
    if (!r) return false;
    if (r->id == 1) return false;
    if (w_.party && w_.party->party_member(r->id)) return false;
    return true;
}

bool ContainerGump::take_button(int& bx, int& by, int& bw, int& bh) const {
    if (!foreign() || !w_.take_item_ui) return false;
    int sx, sy, sw, sh;
    const ShapeFont& f = w_.font(4);
    bw = (f.valid() ? f.text_width("TAKE ALL") : 36) + 9;
    bh = 11;
    if (sort_button(sx, sy, sw, sh)) { bx = std::max(0, sx - bw - 3); by = sy; }
    else { bx = std::max(0, area_l + area_w - bw); by = area_t + area_h + 2; }
    return true;
}

int ContainerGump::take_all() {
    Item* c = w_.item(owner);
    if (!c || !w_.take_item_ui) return 0;
    if (!can_drag_out(0)) { w_.refuse(w_.stasis ? "Not now" : "Too far away: walk closer to it first"); return 0; }
    std::vector<uint16_t> list;
    for (uint16_t k : c->contents)
        if (const Item* it = w_.item(k); it && !(it->flags & (kItemEquipped | kItemInvisible))) list.push_back(k);
    int taken = 0, left = 0;
    std::string why;
    for (uint16_t k : list) {
        if (w_.take_item_ui(k)) { ++taken; continue; }
        ++left;
        if (why.empty()) why = w_.refuse_text;
    }
    if (left) w_.refuse("Took " + std::to_string(taken) + ", left " + std::to_string(left) + ": " + why);
    else if (!taken) w_.refuse("Nothing to take");
    return taken;
}

bool ContainerGump::press(int button, int lx, int ly) {
    int bx, by, bw, bh;
    take_down = button == 1 && take_button(bx, by, bw, bh) && lx >= bx && ly >= by && lx < bx + bw && ly < by + bh;
    sort_down = button == 1 && sort_button(bx, by, bw, bh) && lx >= bx && ly >= by && lx < bx + bw && ly < by + bh;
    return true;
}

void ContainerGump::release(int button, int lx, int ly) {
    if (take_down) {
        take_down = false;
        int bx, by, bw, bh;
        if (button == 1 && take_button(bx, by, bw, bh) && lx >= bx && ly >= by && lx < bx + bw && ly < by + bh) take_all();
        return;
    }
    if (!sort_down) return;
    sort_down = false;
    int bx, by, bw, bh;
    if (button == 1 && sort_button(bx, by, bw, bh) && lx >= bx && ly >= by && lx < bx + bw && ly < by + bh)
        sort_container(w_, *this);
}

void ContainerGump::paint(Surface8& s) {
    Gump::paint(s);
    const Item* c = w_.item(owner);
    if (!c) return;
    s.set_clip(x + area_l, y + area_t, area_w, area_h);
    for (uint16_t k : c->contents) {
        const Item* it = w_.item(k);
        if (!it || k == hidden_item || (it->flags & (kItemEquipped | kItemInvisible))) continue;
        const ShapeFrame* f = w_.data().shapes.frame(it->shape, it->frame);
        if (!f) continue;
        s.draw(*f, x + area_l + (it->y & 0xFF), y + area_t + ((it->y >> 8) & 0xFF), it->flags & kItemFlipped);
    }
    if (ghost_item_)
        if (const Item* it = w_.item(ghost_item_))
            if (const ShapeFrame* f = w_.data().shapes.frame(it->shape, it->frame))
                s.draw(*f, x + ghost_x_, y + ghost_y_, it->flags & kItemFlipped, nullptr, ghost_table(w_).data());
    s.reset_clip();
    int bx, by, bw, bh;
    if (sort_button(bx, by, bw, bh)) paint_sort_button(s, w_, gump_frame(42, sort_down ? 1 : 0), x + bx, y + by, bw, sort_down);
    if (take_button(bx, by, bw, bh)) paint_sort_button(s, w_, gump_frame(42, take_down ? 1 : 0), x + bx, y + by, bw, take_down, "TAKE ALL");
}

uint16_t ContainerGump::trace_item(int lx, int ly) const {
    const Item* c = w_.item(owner);
    if (!c) return 0;
    for (size_t i = c->contents.size(); i-- > 0;) {
        uint16_t k = c->contents[i];
        const Item* it = w_.item(k);
        if (!it || k == hidden_item || (it->flags & (kItemEquipped | kItemInvisible))) continue;
        const ShapeFrame* f = w_.data().shapes.frame(it->shape, it->frame);
        if (!f) continue;
        int ax = area_l + (it->y & 0xFF), ay = area_t + ((it->y >> 8) & 0xFF);
        int col = (it->flags & kItemFlipped) ? ax + f->xoff - lx : lx - (ax - f->xoff), row = ly - (ay - f->yoff);
        if (lx >= area_l && ly >= area_t && lx < area_l + area_w && ly < area_t + area_h && f->opaque(col, row)) return k;
    }
    return 0;
}

bool ContainerGump::item_anchor(uint16_t item, int& sx, int& sy) const {
    const Item* it = w_.item(item);
    if (!it || it->parent != owner) return false;
    sx = x + area_l + (it->y & 0xFF);
    sy = y + area_t + ((it->y >> 8) & 0xFF);
    return true;
}

bool ContainerGump::hit(int lx, int ly) const {
    int bx, by, bw, bh;
    if (sort_button(bx, by, bw, bh) && lx >= bx && ly >= by && lx < bx + bw && ly < by + bh) return true;
    if (take_button(bx, by, bw, bh) && lx >= bx && ly >= by && lx < bx + bw && ly < by + bh) return true;
    return Gump::hit(lx, ly) || trace_item(lx, ly) != 0;
}

void ContainerGump::click(int button, int lx, int ly) {
    if (button != 1 || w_.stasis) return;
    if (uint16_t id = trace_item(lx, ly)) w_.call_event(id, kEvLook);
}

void ContainerGump::double_click(int button, int lx, int ly) {
    if (button != 1 || w_.stasis) return;
    int bx, by, bw, bh;
    if (sort_button(bx, by, bw, bh) && lx >= bx && ly >= by && lx < bx + bw && ly < by + bh) return;  // not "use"
    if (take_button(bx, by, bw, bh) && lx >= bx && ly >= by && lx < bx + bw && ly < by + bh) return;
    uint16_t id = trace_item(lx, ly);
    // Someone else's container: an item (not a container, which opens) goes into the backpack.
    if (id && foreign() && w_.take_item_ui) {
        const Item* it = w_.item(id);
        if (it && w_.data().typeflags.get(it->shape).family() != kFamContainer) {
            if (!can_drag_out(id)) w_.refuse("Too far away: walk closer to it first");
            else w_.take_item_ui(id);
            return;
        }
    }
    if (!id) {
        // The gump itself: "use" the container, which normally closes it.
        if (const Actor* a = w_.actor(owner); a && a->is(Actor::kDead)) { close(); return; }
        if (owner == 1) return;
        w_.use_item(owner);
        return;
    }
    w_.use_item(id);
}

bool ContainerGump::can_drag_out(uint16_t item) const {
    if (w_.stasis) return false;
    return w_.can_reach(1, owner, 128);
}

Gump::Drop ContainerGump::drag_over(uint16_t item, int lx, int ly, int gx, int gy) {
    ghost_item_ = 0;
    drop_why.clear();
    const Item* it = w_.item(item);
    if (!it) return kDropNo;
    const ShapeFrame* f = w_.data().shapes.frame(it->shape, it->frame);
    if (!f) return kDropNo;
    int ax = lx - gx, ay = ly - gy;  // where the item's anchor would be
    int l = (it->flags & kItemFlipped) ? ax + f->xoff - (f->w - 1) : ax - f->xoff, t = ay - f->yoff;
    // Milestone 6: a drop anywhere on the window goes in; the item is moved inside the area
    // (centred if it is bigger than the area).
    int nl = f->w >= area_w ? area_l + (area_w - f->w) / 2 : std::clamp(l, area_l, area_l + area_w - f->w);
    int nt = f->h >= area_h ? area_t + (area_h - f->h) / 2 : std::clamp(t, area_t, area_t + area_h - f->h);
    ax += nl - l;
    ay += nt - t;
    uint16_t target = owner;
    if (uint16_t under = trace_item(lx, ly); under && under != item) {
        const Item* u = w_.item(under);
        const ShapeInfo& si = w_.data().typeflags.get(u->shape);
        if (si.family() == kFamContainer && !si.is(ShapeInfo::kLand) && !u->as_actor()) target = under;
    }
    drop_why = w_.put_in_refusal(item, target);
    if (!drop_why.empty()) return kDropNo;
    ghost_target_ = target;
    ghost_item_ = item;
    ghost_x_ = ax;
    ghost_y_ = ay;
    return kDropInto;
}

bool ContainerGump::drop(uint16_t item, int lx, int ly, int gx, int gy) {
    Drop d = drag_over(item, lx, ly, gx, gy);
    ghost_item_ = 0;
    if (d == kDropNo) return false;
    Item* it = w_.item(item);
    if (!it) return false;
    if (ghost_target_ != owner) {
        bool already = it->parent == ghost_target_;
        if (!w_.move_to_container(item, ghost_target_, true)) return false;
        if (already) {  // just to the top of its drawing order
            if (Item* c = w_.item(ghost_target_)) {
                auto pos = std::find(c->contents.begin(), c->contents.end(), item);
                if (pos != c->contents.end()) { c->contents.erase(pos); c->contents.push_back(item); }
            }
        }
        return true;
    }
    int px = std::clamp(ghost_x_ - area_l, 0, 254), py = std::clamp(ghost_y_ - area_t, 0, 254);
    if (it->parent == owner && !(it->flags & kItemEquipped)) {
        Item* c = w_.item(owner);
        auto pos = std::find(c->contents.begin(), c->contents.end(), item);
        if (pos != c->contents.end()) { c->contents.erase(pos); c->contents.push_back(item); }
    } else if (!w_.move_to_container(item, owner, true)) {
        return false;
    }
    if ((it = w_.item(item))) it->y = (py << 8) | px;
    return true;
}

void ContainerGump::on_close() {
    Item* c = w_.item(owner);
    if (!c) return;
    c->flags &= uint16_t(~kItemGumpOpen);
    // Contents leave the fast area; gumps of containers inside close too (gumps.md 2.6).
    std::vector<uint16_t> inner = c->contents;
    for (uint16_t k : inner) {
        if (Gump* g = w_.gumps.container_gump(k)) g->close();
        Item* it = w_.item(k);
        if (it && (it->flags & kItemFastArea)) {
            w_.item_leave_fast(*it);
            if ((it = w_.item(k))) it->flags &= uint16_t(~kItemFastArea);
        }
    }
}

void ContainerGump::save(SaveWriter& out) const {
    Gump::save(out);
    out.i32(off_x); out.i32(off_y); out.b(placed);
}

void ContainerGump::load(SaveReader& in) {
    Gump::load(in);
    off_x = in.i32(); off_y = in.i32(); placed = in.b();
}

// ---------------------------------------------------------------------------------------------
// Paperdoll (gumps.md 2.5)

namespace {
const int kPdAreaX = 7, kPdAreaY = 9;
const int kBpX = 49, kBpY = 25, kBpW = 59, kBpH = 50;  // backpack hot spot, item-area coords
const int kStatsBtnX = 81, kStatsBtnY = 84;
}  // namespace

PaperdollGump::PaperdollGump(World& w, uint16_t actor) : ContainerGump(w, actor, 1) {}

void PaperdollGump::slot_pos(unsigned slot, int& x, int& y) {
    static const int p[7][2] = {{0, 0}, {24, 60}, {36, 50}, {40, 26}, {40, 63}, {40, 92}, {16, 18}};
    x = slot < 7 ? p[slot][0] : 0;
    y = slot < 7 ? p[slot][1] : 0;
}

bool PaperdollGump::over_backpack(int lx, int ly) const {
    int ix = lx - kPdAreaX, iy = ly - kPdAreaY;
    return ix >= kBpX && iy >= kBpY && ix < kBpX + kBpW && iy < kBpY + kBpH;
}

void PaperdollGump::paint(Surface8& s) {
    // Companions (game/party.h): the same picture with the body in their colours, their name.
    const uint8_t* doll = (owner != 1 && w_.party) ? w_.party->doll_remap(owner) : nullptr;
    const ShapeFrame* own = (owner != 1 && w_.party) ? w_.party->doll_picture(owner) : nullptr;
    if (own) {
        s.draw(*own, x + own->xoff, y + own->yoff);
    } else if (doll) {
        if (const ShapeFrame* f = gump_frame(shape, frame)) s.draw(*f, x + f->xoff, y + f->yoff, false, nullptr, nullptr, doll);
    } else {
        Gump::paint(s);
    }
    const Actor* a = w_.actor(owner);
    if (!a) return;
    if (doll) {
        std::string n = w_.party->display_name(owner);
        const ShapeFont& nf = w_.font(7);
        if (nf.text_width(n) > 64 && n.find(' ') != std::string::npos) n = n.substr(0, n.find(' '));
        nf.draw(s, x + 117 - nf.text_width(n) / 2, y + 15, n);
    }
    // Equipment, slot 6 first so shields end on top; each with frame + 1.
    for (unsigned slot = 6; slot >= 1; --slot) {
        uint16_t id = w_.equipped(owner, slot);
        const Item* it = w_.item(id);
        if (!it || id == hidden_item) continue;
        const ShapeFrame* f = w_.data().shapes.frame(it->shape, it->frame + 1u);
        if (!f) continue;
        int px, py;
        slot_pos(slot, px, py);
        s.draw(*f, x + kPdAreaX + px, y + kPdAreaY + py);
    }
    if (ghost_item_)
        if (const Item* it = w_.item(ghost_item_)) {
            unsigned f = ghost_target_ == owner ? it->frame + 1u : it->frame;
            if (const ShapeFrame* fr = w_.data().shapes.frame(it->shape, f))
                s.draw(*fr, x + ghost_x_, y + ghost_y_, false, nullptr, ghost_table(w_).data());
        }
    // Stats (baseline positions).
    static const char* labels[7] = {"STR", "INT", "DEX", "ARMR", "HITS", "MANA", "WGHT"};
    int armour = armour_class(w_, *a);
    uint32_t weight = w_.total_weight(*a) - (doll ? w_.item_weight(*a) : 0);
    if (const Item* g = w_.item(ghost_item_)) {
        // While an item is dragged over, show what the Avatar would have (gumps.md 2.5).
        if (w_.root_container(*g)->id != owner) weight += w_.total_weight(*g);
        if (ghost_target_ == owner)
            armour += item_armour(*g);
    }
    int vals[7] = {a->str, a->intel, a->dex, armour, int(a->hp), a->mana, int(weight / 10)};
    const ShapeFont& lf = w_.font(0);
    const ShapeFont& vf = w_.font(7);
    for (int n = 0; n < 7; ++n) {
        int by = y + 24 + 9 * n;
        lf.draw(s, x + 90 + 29 - lf.text_width(labels[n]), by, labels[n]);
        std::string v = std::to_string(vals[n]);
        vf.draw(s, x + 130 + 15 - vf.text_width(v), by, v);
    }
    draw_shape(s, 38, stats_down_ ? 1 : 0, kStatsBtnX, kStatsBtnY);
    // Engine extras (game/sheet_extras.cpp): the level line under the stats button, the Avatar's
    // virtue octagon, and the note a click on them shows.
    paint_level_line(s);
    paint_octagon(s);
    paint_note(s);
}

bool PaperdollGump::hit(int lx, int ly) const {
    if (Gump::hit(lx, ly)) return true;
    if (const ShapeFrame* f = gump_frame(38, 0); f && f->opaque(lx - kStatsBtnX, ly - kStatsBtnY)) return true;
    return trace_item(lx, ly) != 0;
}

uint16_t PaperdollGump::trace_item(int lx, int ly) const {
    for (unsigned slot = 1; slot <= 6; ++slot) {
        uint16_t id = w_.equipped(owner, slot);
        const Item* it = w_.item(id);
        if (!it || id == hidden_item) continue;
        const ShapeFrame* f = w_.data().shapes.frame(it->shape, it->frame + 1u);
        if (!f) continue;
        int px, py;
        slot_pos(slot, px, py);
        int col = lx - (kPdAreaX + px - f->xoff), row = ly - (kPdAreaY + py - f->yoff);
        if (f->opaque(col, row)) return id;
    }
    if (over_backpack(lx, ly)) return w_.equipped(owner, 7);
    return 0;
}

bool PaperdollGump::item_anchor(uint16_t item, int& sx, int& sy) const {
    const Item* it = w_.item(item);
    if (!it || it->parent != owner || !(it->flags & kItemEquipped)) return false;
    int px, py;
    slot_pos(unsigned(it->z), px, py);
    sx = x + kPdAreaX + px;
    sy = y + kPdAreaY + py;
    return true;
}

bool PaperdollGump::press(int button, int lx, int ly) {
    const ShapeFrame* f = gump_frame(38, 0);
    stats_down_ = button == 1 && f && lx >= kStatsBtnX && ly >= kStatsBtnY && lx < kStatsBtnX + f->w && ly < kStatsBtnY + f->h;
    return true;
}

void PaperdollGump::release(int button, int lx, int ly) {
    if (!stats_down_) return;
    stats_down_ = false;
    const ShapeFrame* f = gump_frame(38, 0);
    if (!f || lx < kStatsBtnX || ly < kStatsBtnY || lx >= kStatsBtnX + f->w || ly >= kStatsBtnY + f->h) return;
    // The stats button: the mini-stats gump at the bottom-right corner (gumps.md 2.7).
    MiniStatsGump* m = w_.gumps.find<MiniStatsGump>();
    if (!m) m = static_cast<MiniStatsGump*>(w_.gumps.add(std::make_unique<MiniStatsGump>(w_)));
    m->x = 320 - 5 - m->w;
    m->y = 200 - 5 - m->h;
}

void PaperdollGump::double_click(int button, int lx, int ly) {
    if (button != 1 || w_.stasis) return;
    if (uint16_t id = trace_item(lx, ly)) w_.use_item(id);
}

bool PaperdollGump::can_drag_out(uint16_t item) const {
    const Item* it = w_.item(item);
    return !w_.stasis && it && it->shape != 529;
}

Gump::Drop PaperdollGump::drag_over(uint16_t item, int lx, int ly, int gx, int gy) {
    ghost_item_ = 0;
    drop_why.clear();
    const Item* it = w_.item(item);
    if (!it || it->shape == 529) return kDropNo;
    int ix = lx - kPdAreaX, iy = ly - kPdAreaY;
    GameData::GumpArea a = w_.data().gump_area(1);
    // Milestone 6: on the figure an equippable item is equipped; anywhere else on the
    // paperdoll (the backpack picture, the frame, the stats) it goes into the backpack.
    bool on_figure = lx >= a.left && ly >= a.top && lx < a.right && ly < a.bottom;
    (void)ix; (void)iy;
    unsigned slot = w_.data().typeflags.get(it->shape).equip_type();
    uint16_t pack = w_.equipped(owner, 7);
    if (on_figure && slot && slot <= 6 && !over_backpack(lx, ly) && w_.can_equip(owner, item)) {
        int px, py;
        slot_pos(slot, px, py);
        ghost_item_ = item;
        ghost_target_ = owner;
        ghost_x_ = kPdAreaX + px;
        ghost_y_ = kPdAreaY + py;
        return kDropPlace;
    }
    drop_why = pack ? w_.put_in_refusal(item, pack) : "No backpack";
    if (!drop_why.empty()) return kDropNo;
    ghost_item_ = item;
    ghost_target_ = pack;
    ghost_x_ = kPdAreaX + kBpX + kBpW / 2;
    ghost_y_ = kPdAreaY + kBpY + kBpH / 2;
    return kDropInto;
}

bool PaperdollGump::drop(uint16_t item, int lx, int ly, int gx, int gy) {
    Drop d = drag_over(item, lx, ly, gx, gy);
    uint16_t target = ghost_target_;
    ghost_item_ = 0;
    if (d == kDropNo) return false;
    if (target == owner) return w_.equip(owner, item);
    if (Item* it = w_.item(item); it && it->parent == target) return true;
    return w_.move_to_container(item, target, true);
}

// ---------------------------------------------------------------------------------------------
// Mini-stats (gumps.md 2.7)

MiniStatsGump::MiniStatsGump(World& w) : Gump(w) {
    shape = 33;
    draggable = true;
    if (const ShapeFrame* f = gump_frame(33, 0)) { this->w = f->w; h = f->h; }
    else { this->w = 22; h = 26; }
    x = 320 - 5 - this->w;
    y = 200 - 5 - h;
}

void MiniStatsGump::paint(Surface8& s) {
    Gump::paint(s);
    const Actor* a = w_.avatar();
    if (!a) return;
    auto bar = [&](int x0, int val, int maxv, const uint8_t cols[3]) {
        int hgt = maxv > 0 ? std::clamp(val * 14 / maxv, 0, 14) : 0;
        for (int c = 0; c < 3; ++c) s.fill_rect(x + x0 + c, y + 19 - hgt + 1, 1, hgt, cols[c]);
    };
    static const uint8_t red[3] = {41, 39, 37}, blue[3] = {138, 139, 141};
    bar(6, a->hp, std::max(1, int(a->str) * 2), red);
    bar(13, a->mana, std::max(0, int(a->intel) * 2), blue);
}

void MiniStatsGump::double_click(int button, int lx, int ly) {
    if (button != 1) return;
    if (!w_.gumps.container_gump(1)) w_.call_event(1, kEvUse);
    close();
}

// ---------------------------------------------------------------------------------------------
// Slider (gumps.md 2.10)

namespace {
const int kKnobY = 17, kKnobMin = 55, kKnobMax = 140;
const int kLeftX = 36, kRightX = 141, kOkX = 158, kPartY = 17;
}  // namespace

SliderGump::SliderGump(World& w, int min, int max, int step, int value)
    : Gump(w), min_(min), max_(std::max(min, max)), step_(std::max(1, step)), value_(std::clamp(value, min, std::max(min, max))) {
    shape = 41;
    modal = true;
    pauses = true;
    layer = kLayerModal;
    cursor = kCursorHand;
    if (const ShapeFrame* f = gump_frame(41, 0)) { this->w = f->w; h = f->h; }
    else { this->w = 194; h = 47; }
    x = 100;
    y = 100;
}

int SliderGump::knob_x() const {
    const ShapeFrame* k = gump_frame(45, 0);
    int kw = k ? k->w : 10;
    if (max_ == min_) return kKnobMin;
    return kKnobMin + (value_ - min_) * (kKnobMax - kKnobMin - kw) / (max_ - min_);
}

void SliderGump::set_value(int v) { value_ = std::clamp(v, min_, max_); }

void SliderGump::paint(Surface8& s) {
    Gump::paint(s);
    draw_shape(s, 45, 0, knob_x(), kKnobY);
    draw_shape(s, 43, 0, kLeftX, kPartY);
    draw_shape(s, 44, 0, kRightX, kPartY);
    draw_shape(s, 42, ok_down_ ? 1 : 0, kOkX, kPartY);
    w_.font(0).draw(s, x + 18, y + 26, std::to_string(value_));
}

bool SliderGump::press(int button, int lx, int ly) {
    if (button != 1) return true;
    auto in = [&](unsigned sh, int px, int py) {
        const ShapeFrame* f = gump_frame(sh, 0);
        return f && lx >= px && ly >= py && lx < px + f->w && ly < py + f->h;
    };
    press_part_ = 0;
    if (in(45, knob_x(), kKnobY)) { press_part_ = 1; dragging_ = true; }
    else if (in(43, kLeftX, kPartY)) press_part_ = 2;
    else if (in(44, kRightX, kPartY)) press_part_ = 3;
    else if (in(42, kOkX, kPartY)) { press_part_ = 4; ok_down_ = true; }
    return true;
}

void SliderGump::hover(int lx, int ly) {
    if (!dragging_) return;
    const ShapeFrame* k = gump_frame(45, 0);
    int kw = k ? k->w : 10;
    int span = kKnobMax - kKnobMin - kw;
    if (span <= 0 || max_ == min_) return;
    int kx = std::clamp(lx - kw / 2, kKnobMin, kKnobMax - kw);
    int v = min_ + (kx - kKnobMin) * (max_ - min_) / span;
    v = min_ + step_ * ((v - min_) / step_);
    set_value(v);
}

void SliderGump::release(int button, int lx, int ly) {
    auto in = [&](unsigned sh, int px, int py) {
        const ShapeFrame* f = gump_frame(sh, 0);
        return f && lx >= px && ly >= py && lx < px + f->w && ly < py + f->h;
    };
    if (press_part_ == 2 && in(43, kLeftX, kPartY)) set_value(value_ - step_);
    if (press_part_ == 3 && in(44, kRightX, kPartY)) set_value(value_ + step_);
    if (press_part_ == 4 && in(42, kOkX, kPartY)) accept();
    dragging_ = false;
    ok_down_ = false;
    press_part_ = 0;
}

bool SliderGump::key(int key, unsigned mods) {
    if (key == kKeyLeft) set_value(value_ - step_);
    else if (key == kKeyRight) set_value(value_ + step_);
    else if (key == kKeyReturn) accept();
    return true;  // modal: swallows keys (Escape does nothing special)
}

void SliderGump::on_close() {
    if (wake_pid)
        if (auto* p = dynamic_cast<UCProcess*>(w_.kernel().get(wake_pid)); p && p->live()) {
            p->temp = uint32_t(uint16_t(value_));
            p->wake(uint32_t(uint16_t(value_)));
        }
    if (done) done(accepted_ ? value_ : 0);
}

// ---------------------------------------------------------------------------------------------
// Readers (gumps.md 2.11)

ReaderGump::ReaderGump(World& w, Kind k, uint16_t item, unsigned gump_shape, const std::string& text)
    : Gump(w), reader_kind(k) {
    owner = item;
    shape = gump_shape;
    modal = true;
    pauses = true;
    layer = kLayerModal;
    cursor = kCursorHand;
    const ShapeFrame* gf = gump_frame(shape, 0);
    this->w = gf ? gf->w : 240;
    h = gf ? gf->h : 140;
    x = (320 - this->w) / 2;
    y = (200 - h) / 2;
    switch (k) {
    case kBook: font = 9; pages = layout_text(w.font(9), text, 123, 129, true); break;
    case kScroll: font = 9; pages = layout_text(w.font(9), text, 204, 115, true); break;
    case kGrave: case kPlaque:
        font = k == kGrave ? 11 : 10;
        pages = layout_text(w.font(font), text, std::max(40, this->w - 16), 0);
        break;
    }
    if (pages.empty()) pages.emplace_back();
}

void ReaderGump::paint(Surface8& s) {
    if (shape && gump_frame(shape, 0)) Gump::paint(s);
    else s.fill_rect(x, y, w, h, 0);
    const ShapeFont& f = w_.font(font);
    auto draw_page = [&](unsigned p, int px, int py, int boxw, bool centred) {
        if (p >= pages.size()) return;
        const TextPage& pg = pages[p];
        for (size_t i = 0; i < pg.lines.size(); ++i) {
            int lx = centred ? px + (boxw - f.text_width(pg.lines[i])) / 2 : px;
            f.draw_top(s, lx, py + int(i) * f.line_advance(), pg.lines[i]);
        }
    };
    switch (reader_kind) {
    case kBook:
        draw_page(page, x + 9, y + 5, 123, false);
        draw_page(page + 1, x + 150, y + 5, 123, false);
        break;
    case kScroll: draw_page(page, x + 22, y + 29, 204, false); break;
    default: {
        int th = pages[page].height;
        draw_page(page, x + 8, y + (h - th) / 2, w - 16, true);
        break;
    }
    }
}

void ReaderGump::advance() {
    unsigned step = reader_kind == kBook ? 2 : 1;
    if (reader_kind != kGrave && reader_kind != kPlaque && page + step < pages.size()) {
        page += step;
        return;
    }
    close();
}

bool ReaderGump::press(int button, int lx, int ly) {
    if (reader_kind == kGrave || reader_kind == kPlaque) close();  // any press closes
    return true;
}

void ReaderGump::click(int button, int lx, int ly) {
    if (reader_kind == kBook || reader_kind == kScroll) advance();
}

void ReaderGump::double_click(int button, int lx, int ly) { close(); }

bool ReaderGump::turn(int dir) {
    if (!paged()) return false;
    int step = reader_kind == kBook ? 2 : 1;
    int to = int(page) + (dir < 0 ? -step : step);
    if (to < 0 || to >= int(pages.size())) return false;
    page = unsigned(to);
    return true;
}

bool ReaderGump::key(int key, unsigned mods) {
    if (reader_kind == kGrave || reader_kind == kPlaque) { close(); return true; }
    if (key == kKeySpace || key == kKeyReturn) advance();
    else if (key == kKeyEscape) close();
    else if (key == kKeyLeft || key == kKeyPageUp) turn(-1);
    else if (key == kKeyRight || key == kKeyPageDown) turn(1);
    return true;
}

void ReaderGump::tick() {
    if (w_.auto_ui && ++frames_open % 60 == 0) advance();
}

// ---------------------------------------------------------------------------------------------
// Quit box (gumps.md 2.15)

QuitGump::QuitGump(World& w) : Gump(w) {
    shape = 17;
    modal = true;
    pauses = true;
    layer = kLayerModal;
    cursor = kCursorHand;
    const ShapeFrame* f = gump_frame(17, 0);
    this->w = f ? f->w : 125;
    h = f ? f->h : 67;
    x = (320 - this->w) / 2;
    y = (200 - h) / 2;
}

void QuitGump::paint(Surface8& s) {
    Gump::paint(s);
    if (const ShapeFrame* q = gump_frame(18, 0)) draw_shape(s, 18, 0, (w - q->w) / 2, 5);
    draw_shape(s, 47, down_ == 1 ? 1 : 0, 16, 38);
    if (const ShapeFrame* n = gump_frame(50, 0)) draw_shape(s, 50, down_ == 2 ? 1 : 0, w - 16 - n->w, 38);
}

namespace {
bool in_frame(const ShapeFrame* f, int px, int py, int lx, int ly) {
    return f && lx >= px && ly >= py && lx < px + f->w && ly < py + f->h;
}
}  // namespace

bool QuitGump::press(int button, int lx, int ly) {
    const ShapeFrame* y1 = gump_frame(47, 0);
    const ShapeFrame* n1 = gump_frame(50, 0);
    down_ = 0;
    if (in_frame(y1, 16, 38, lx, ly)) down_ = 1;
    else if (n1 && in_frame(n1, w - 16 - n1->w, 38, lx, ly)) down_ = 2;
    return true;
}

void QuitGump::release(int button, int lx, int ly) {
    const ShapeFrame* y1 = gump_frame(47, 0);
    const ShapeFrame* n1 = gump_frame(50, 0);
    if (down_ == 1 && in_frame(y1, 16, 38, lx, ly)) w_.request.kind = World::Request::kQuit;
    if (down_ == 2 && n1 && in_frame(n1, w - 16 - n1->w, 38, lx, ly)) close();
    down_ = 0;
}

bool QuitGump::key(int key, unsigned mods) {
    if (key == 'y') w_.request.kind = World::Request::kQuit;
    else if (key == 'n' || key == kKeyEscape) close();
    return true;
}

// ---------------------------------------------------------------------------------------------
// The diary book: main menu, name entry, save / load and options (gumps.md 2.2-2.4)

namespace {
const int kBookW = 307, kBookH = 158;
const int kSpreadX = (kBookW - 220) / 2, kSpreadY = 6;
const int kSpreads = 16;
const int kColX[2] = {3, 122}, kRowY[3] = {3, 43, 83};
}  // namespace

// The options page (option_rows, layout, painting and input) is in game/options_page.cpp.

// The dog-ears (shape 34; frame 1 is frame 0 mirrored). The left one sits at (34, 3): its tip
// on the left page's outer edge (the first parchment column of the book picture is 33). The
// spec's place for the right one (38 from the right edge, gumps.md 2.4) puts it beside the page,
// over the gilt edge and the cover, so it seemed to curl the wrong way; it is drawn at the
// mirror place instead, its tip on the right page's outer edge (the last parchment column is
// w - 39), so the two corners mirror each other.
void MenuGump::dogear_box(int which, int& dx, int& dy, int& dw, int& dh) const {
    const ShapeFrame* f = gump_frame(34, which ? 1 : 0);
    dw = f ? f->w : 24;
    dh = f ? f->h : 20;
    dx = which ? w - 38 - dw : 34;
    dy = 3;
}

bool MenuGump::dogear_hit(int which, int lx, int ly) const {
    int dx, dy, dw, dh;
    dogear_box(which, dx, dy, dw, dh);
    const ShapeFrame* f = gump_frame(34, which ? 1 : 0);
    if (!f) return lx >= dx && ly >= dy && lx < dx + dw && ly < dy + dh;
    constexpr int kSlack = 2;  // the corner is thin at its tip: a near miss still turns
    for (int oy = -kSlack; oy <= kSlack; ++oy)
        for (int ox = -kSlack; ox <= kSlack; ++ox)
            if (f->opaque(lx - dx + ox, ly - dy + oy)) return true;  // drawn with its top-left at (dx, dy)
    return false;
}

void MenuGump::draw_dogear(Surface8& s, int which) const {
    int dx, dy, dw, dh;
    dogear_box(which, dx, dy, dw, dh);
    draw_shape(s, 34, which ? 1 : 0, dx, dy);
}

MenuGump::MenuGump(World& w, Mode m) : Gump(w), mode_(m) {
    shape = 35;
    modal = true;
    pauses = true;
    layer = kLayerModal;
    this->w = kBookW;
    h = kBookH;
    if (const ShapeFrame* f = gump_frame(35, 0)) { this->w = f->w; h = f->h; }
    x = (320 - this->w) / 2;
    y = (200 - h) / 2;
    set_mode(m);
    // The music state is saved and the music stops while the book is open (gumps.md 2.2).
    w.sound.menu_opened();
    // The world's palette transform is reset while the book is open (restored on close).
    w.menu_saved_palette = w.palette;
    w.palette = PaletteTransform{};
}

void MenuGump::set_mode(Mode m) {
    mode_ = m;
    editing = 0;
    hover_ = -1;
    sel_ = -1;
    sel_mouse_ = false;
    down_item_ = -1;
    mode_tick_ = caret_ticks_;
    mode_ticks_ = ticks_;
    edit_touched = false;
    if (m == kMenu) sel_ = default_item();
    if (m == kOptions) { option_spread = 0; option_hover_ = -1; option_sel_ = -1; select_option(-1); }
    switch (m) {
    case kMenu: case kOptions: cursor = kCursorHand; break;
    case kName: cursor = kCursorHidden; edit_text.clear(); break;
    case kLoad: cursor = kCursorLens; break;
    case kSave: cursor = kCursorQuill; break;
    }
    if ((m == kLoad || m == kSave) && w_.saves) {
        w_.saves->refresh();
        // Read Diary opens where the most recent save is (an autosave: the autosave page) with it
        // highlighted; Write Diary on the first free entry after the newest diary entry, ready
        // to type (NOTES.md "Start menu, menu clicks and the icon").
        int at = 0;
        if (m == kLoad) {
            at = w_.saves->latest();
            if (at >= SaveStore::kAutoFirst) spread = -1;
            else if (at > 1) spread = (at - 1) / 6;
            else spread = min_spread();
        } else {
            at = next_free_slot();
            spread = (at - 1) / 6;
        }
        spread = std::clamp(spread, min_spread(), kSpreads - 1);
        std::vector<int> slots = spread_slots();
        diary_sel = std::find(slots.begin(), slots.end(), at) != slots.end() ? at : slots.empty() ? 0 : slots.front();
        if (m == kSave && diary_sel > 1) pick_slot(diary_sel);  // the caret is there: just type
    }
}

int MenuGump::next_free_slot() const {
    if (!w_.saves) return 2;
    int newest = w_.saves->latest_diary();
    for (int sl = std::max(2, newest + 1); sl <= SaveStore::kSlots; ++sl)
        if (!w_.saves->exists(sl)) return sl;
    for (int sl = 2; sl <= SaveStore::kSlots; ++sl)
        if (!w_.saves->exists(sl)) return sl;
    return std::max(2, newest);  // the diary is full: offer the newest entry again
}

void MenuGump::turn_diary(int dir) {
    int to = std::clamp(spread + (dir < 0 ? -1 : 1), min_spread(), kSpreads - 1);
    if (to == spread) return;
    std::vector<int> old = spread_slots();
    int at = int(std::find(old.begin(), old.end(), diary_sel) - old.begin());
    if (editing) editing = 0;  // the entry being written goes off the page: the old text is kept
    spread = to;
    std::vector<int> ns = spread_slots();
    if (!ns.empty()) diary_sel = ns[size_t(std::clamp(at < int(old.size()) ? at : 0, 0, int(ns.size()) - 1))];
    PG_LOG("diary: spread %d", spread);
}

void MenuGump::on_close() {
    w_.palette = w_.menu_saved_palette;
    w_.sound.menu_closed();
}

bool MenuGump::entry_visible(int i) const {
    if (i == 6) return Config::global().get_bool("quotes_seen", false);
    if (i == 7) return Config::global().get_bool("endgame_seen", false);
    return i >= 0 && i < 6;
}

void MenuGump::tick() {
    ++caret_ticks_;
    ++ticks_;
    if (mode_ == kName && w_.auto_ui && caret_ticks_ > 20) {
        w_.avatar_name = edit_text.empty() ? "Avatar" : edit_text;
        if (Actor* a = w_.avatar()) a->name = w_.avatar_name;
        close();
    }
}

// Milestone 6: two text entries under the picture entries, "Map (M)" and "Controls (F1)",
// when the game mode provides them.
namespace { constexpr int kExtraY = 136, kExtraX[2] = {170, 228}, kExtraW[2] = {52, 72}; }

int MenuGump::extra_at(int lx, int ly) const {
    if (!w_.open_map_ui || ly < kExtraY - 1 || ly >= kExtraY + 12) return -1;
    for (int i = 0; i < 2; ++i)
        if (lx >= kExtraX[i] && lx < kExtraX[i] + kExtraW[i]) return i;
    return -1;
}

void MenuGump::choose_extra(int i) {
    std::function<void()> f = i == 0 ? w_.open_map_ui : w_.open_help_ui;
    if (!f) return;
    close();
    f();
}

namespace {
const char* const kExtraText[2] = {"Map (M)", "Controls (F1)"};
const char* const kDeathText = "Load last autosave (Enter)";
const char* const kContinueText = "Continue (Enter)";
const char* const kNewGameText = "New Game (N)";
// The start menu's lines and the name above them (left page, centred on x = 90).
constexpr int kStartNameY = 108, kContinueY = 123, kNewGameY = 135;
}  // namespace

bool MenuGump::text_line_box(int item, int& bx, int& by, int& bw, int& bh) const {
    const ShapeFont& f = w_.font(4);
    const char* t = nullptr;
    int cx = -1;  // centred on cx, else left at bx
    bh = f.height() + 2;
    if ((item == kItemMap || item == kItemHelp) && w_.open_map_ui) {
        int i = item - kItemMap;
        t = kExtraText[i];
        bx = kExtraX[i];
        by = kExtraY;
    } else if (item == kItemDeath) {
        int slot;
        if (!death_autosave(slot)) return false;
        t = kDeathText, cx = 90, by = 134;
    } else if (item == kItemContinue && start_menu) {
        t = kContinueText, cx = 90, by = kContinueY;
    } else if (item == kItemNewGame && start_menu) {
        t = kNewGameText, cx = 90, by = kNewGameY;
    }
    if (!t) return false;
    bw = f.text_width(t);
    if (cx >= 0) bx = cx - bw / 2;
    return true;
}

int MenuGump::default_item() const {
    int slot;
    if (death_autosave(slot)) return kItemDeath;  // Enter loads it
    return start_menu ? kItemContinue : -1;
}

std::vector<int> MenuGump::menu_items() const {
    std::vector<int> r;
    int slot;
    if (death_autosave(slot)) r.push_back(kItemDeath);
    if (start_menu) { r.push_back(kItemContinue); r.push_back(kItemNewGame); }
    for (int i = 0; i < 8; ++i)
        if (entry_visible(i)) r.push_back(i);
    if (w_.open_map_ui) r.push_back(kItemMap);
    if (w_.open_help_ui) r.push_back(kItemHelp);
    return r;
}

int MenuGump::menu_item_at(int lx, int ly) const {
    for (int it : menu_items()) {
        if (it < 8) {
            if (in_frame(gump_frame(37, unsigned(2 * it)), 167, 18 + 14 * it, lx, ly)) return it;
            continue;
        }
        int bx, by, bw, bh;
        if (text_line_box(it, bx, by, bw, bh) && lx >= bx - 2 && lx < bx + bw + 2 && ly >= by - 1 && ly < by + bh + 1)
            return it;
    }
    // The death line's old, wider hit box (20..160 x 130..148).
    if (over_death_line(lx, ly)) return kItemDeath;
    return -1;
}

void MenuGump::activate(int item) {
    int slot;
    if (item >= 0 && item < 8) { choose(item + 1); return; }
    switch (item) {
    case kItemMap: choose_extra(0); break;
    case kItemHelp: choose_extra(1); break;
    case kItemDeath:
        if (death_autosave(slot)) w_.request = {World::Request::kLoad, slot, ""};
        break;
    case kItemContinue:
        PG_LOG("menu: continue");
        close();  // the most recent save is already loaded behind the book
        break;
    case kItemNewGame:
        PG_LOG("menu: new game");
        w_.request = {World::Request::kNewGame, World::Request::kNewGameIntro, ""};
        break;
    default: break;
    }
}

void MenuGump::paint_menu(Surface8& s) {
    draw_shape(s, 32, 0, 42, 10);
    for (int i = 0; i < 8; ++i)
        if (entry_visible(i)) draw_shape(s, 37, unsigned(2 * i + (sel_ == i ? 1 : 0)), 167, 18 + 14 * i);
    const ShapeFont& f = w_.font(6);
    f.draw_top(s, x + 90 - f.text_width(w_.avatar_name) / 2, y + (start_menu ? kStartNameY : 118), w_.avatar_name);
    const ShapeFont& e = w_.font(4);
    auto line = [&](int item, const char* t, bool underline) {
        int bx, by, bw, bh;
        if (!text_line_box(item, bx, by, bw, bh)) return;
        e.draw_top(s, x + bx, y + by, t);
        if (underline || sel_ == item) s.fill_rect(x + bx, y + by + e.height() + 1, bw, 1, 0);
        if (sel_ == item && item >= kItemDeath) {  // a small pointer for the keyboard / controller
            e.draw_top(s, x + bx - 2 - e.text_width(">"), y + by, ">");
        }
    };
    if (w_.open_map_ui) {
        line(kItemMap, kExtraText[0], false);
        line(kItemHelp, kExtraText[1], false);
    }
    line(kItemDeath, kDeathText, true);  // the death menu's default choice (app/debug_tools.h)
    line(kItemContinue, kContinueText, false);
    line(kItemNewGame, kNewGameText, false);
}

bool MenuGump::death_autosave(int& slot) const {
    slot = 0;
    const Actor* av = w_.avatar();
    if (!av || !av->is(Actor::kDead) || !w_.saves) return false;
    slot = w_.saves->latest_autosave();
    return slot != 0;
}

bool MenuGump::over_death_line(int lx, int ly) const {
    int slot;
    return death_autosave(slot) && lx >= 20 && lx < 160 && ly >= 130 && ly < 148;
}

int MenuGump::min_spread() const {
    bool quick = w_.saves && w_.saves->exists(SaveStore::kQuickSlot);
    return mode_ == kLoad && (!autosave_order().empty() || quick) ? -1 : 0;
}

std::vector<int> MenuGump::autosave_order() const {
    std::vector<int> r;
    if (!w_.saves) return r;
    int newest = w_.saves->latest_autosave();
    if (!newest) return r;
    // Newest first, then backwards through the rotation.
    for (int k = 0; k < SaveStore::kAutoSlots; ++k) {
        int sl = SaveStore::kAutoFirst + (newest - SaveStore::kAutoFirst - k + SaveStore::kAutoSlots) % SaveStore::kAutoSlots;
        if (w_.saves->exists(sl)) r.push_back(sl);
    }
    return r;
}

void MenuGump::paint_autosaves(Surface8& s) {
    draw_dogear(s, 1);  // the next spread is the diary
    const ShapeFont& f = w_.font(4);
    std::vector<int> order = autosave_order();
    for (size_t i = 0; i < order.size() && i < 3; ++i) {
        int ex = kSpreadX + kColX[0], ey = kSpreadY + kRowY[i];
        if (order[i] == diary_sel) f.draw_top(s, x + ex - 1 - f.text_width(">"), y + ey, ">");
        std::vector<TextPage> pages = layout_text(f, w_.saves->description(order[i]), 95, 38);
        if (pages.empty()) continue;
        for (size_t k = 0; k < pages[0].lines.size(); ++k)
            f.draw_top(s, x + ex, y + ey + int(k) * f.line_advance(), pages[0].lines[k]);
    }
    int rx = kSpreadX + kColX[1];
    // The quick-save (F7 / F8) in the right column's last entry.
    bool quick = w_.saves->exists(SaveStore::kQuickSlot);
    if (quick) {
        int ey = kSpreadY + kRowY[2];
        if (diary_sel == SaveStore::kQuickSlot) f.draw_top(s, x + rx - 1 - f.text_width(">"), y + ey, ">");
        f.draw_top(s, x + rx, y + ey, "Quick save (F8):");
        std::vector<TextPage> pages = layout_text(f, w_.saves->description(SaveStore::kQuickSlot), 95, 38 - f.line_advance());
        if (!pages.empty())
            for (size_t k = 0; k < pages[0].lines.size(); ++k)
                f.draw_top(s, x + rx, y + ey + int(k + 1) * f.line_advance(), pages[0].lines[k]);
    }
    f.draw_top(s, x + rx, y + kSpreadY + 3, "Autosaves");
    std::vector<TextPage> note =
        layout_text(f, "Taken after each step of the story, newest first. The diary follows on the next pages.", 95,
                    quick ? kRowY[2] - 3 - 2 * f.line_advance() : 100);
    if (!note.empty())
        for (size_t k = 0; k < note[0].lines.size(); ++k)
            f.draw_top(s, x + rx, y + kSpreadY + 3 + int(k + 2) * f.line_advance(), note[0].lines[k]);
}

void MenuGump::paint_entry_number(Surface8& s, int n, int px, int py) {
    if (n <= 9) { draw_shape(s, 36, unsigned(n - 1), px, py); return; }
    int tens = n / 10, units = n % 10;
    const ShapeFrame* tf = gump_frame(36, unsigned(tens - 1));
    draw_shape(s, 36, unsigned(tens - 1), px, py);
    int ux = px + (tf ? tf->w : 6);
    if (units == 0) {
        // The 0 is the right part of "10", cut 6 pixels in.
        if (const ShapeFrame* ten = gump_frame(36, 9)) {
            s.set_clip(x + ux, y + py, ten->w - 6, ten->h);
            s.draw(*ten, x + ux - 6 + ten->xoff, y + py + ten->yoff);
            s.reset_clip();
        }
    } else {
        draw_shape(s, 36, unsigned(units - 1), ux, py);
    }
}

void MenuGump::paint_diary(Surface8& s) {
    if (spread < 0) { paint_autosaves(s); return; }
    if (spread > min_spread()) draw_dogear(s, 0);
    if (spread < kSpreads - 1) draw_dogear(s, 1);
    const ShapeFrame* label = gump_frame(46, 0);
    int lw = label ? label->w : 20, lh = label ? label->h : 10;
    const ShapeFont& f = w_.font(4);
    for (int col = 0; col < 2; ++col)
        for (int row = 0; row < 3; ++row) {
            int slot = spread * 6 + col * 3 + row + 1;
            int ex = kSpreadX + kColX[col], ey = kSpreadY + kRowY[row];
            draw_shape(s, 46, 0, ex, ey);
            paint_entry_number(s, slot, ex + 2 + lw, ey);
            if (slot == diary_sel && !editing) f.draw_top(s, x + ex - 1 - f.text_width(">"), y + ey, ">");
            std::string desc;
            if (slot == 1) desc = "The Beginning...";
            else if (slot == editing) desc = edit_text;
            else if (w_.saves) desc = w_.saves->description(slot);
            std::vector<TextPage> pages = layout_text(f, desc, 95, std::max(10, 38 - lh));
            if (!pages.empty()) {
                const TextPage& pg = pages[0];
                for (size_t i = 0; i < pg.lines.size(); ++i)
                    f.draw_top(s, x + ex, y + ey + lh + 1 + int(i) * f.line_advance(), pg.lines[i]);
                if (slot == editing && (caret_ticks_ / 22) % 2 == 0) {
                    int li = int(pg.lines.size()) - 1;
                    int cx = x + ex + (li >= 0 ? f.text_width(pg.lines[size_t(li)]) : 0);
                    s.fill_rect(cx, y + ey + lh + 1 + std::max(0, li) * f.line_advance(), 1, f.height(), 0);
                }
            }
        }
}

void MenuGump::paint(Surface8& s) {
    Gump::paint(s);
    switch (mode_) {
    case kMenu: paint_menu(s); break;
    case kName: {
        draw_shape(s, 32, 0, 42, 10);
        const ShapeFont& f = w_.font(6);
        f.draw_top(s, x + 159, y + 10, "Give thy name:");
        int fy = y + 10 + f.line_advance() + 2;
        f.draw_top(s, x + 159, fy, edit_text);
        if ((caret_ticks_ / 22) % 2 == 0) s.fill_rect(x + 159 + f.text_width(edit_text), fy, 1, f.height(), 0);
        break;
    }
    case kLoad: case kSave: paint_diary(s); break;
    case kOptions: paint_options(s); break;
    }
}

std::vector<int> MenuGump::spread_slots() const {
    std::vector<int> r;
    if (spread < 0) {
        r = autosave_order();
        if (r.size() > 3) r.resize(3);
        if (w_.saves && w_.saves->exists(SaveStore::kQuickSlot)) r.push_back(SaveStore::kQuickSlot);
        return r;
    }
    for (int k = 0; k < 6; ++k) {
        int slot = spread * 6 + k + 1;
        if (mode_ == kSave && slot == 1) continue;  // The Beginning cannot be overwritten
        r.push_back(slot);
    }
    return r;
}

int MenuGump::slot_at(int lx, int ly) const {
    int sx = lx - kSpreadX, sy = ly - kSpreadY;
    int col = sx >= 3 && sx <= 100 ? 0 : sx >= 122 && sx < 220 ? 1 : -1;
    int row = -1;
    for (int r = 0; r < 3; ++r)
        if (sy >= kRowY[r] && sy <= kRowY[r] + 37) row = r;
    if (col < 0 || row < 0) return 0;
    if (spread < 0) {
        std::vector<int> order = autosave_order();
        if (col == 1 && row == 2 && w_.saves && w_.saves->exists(SaveStore::kQuickSlot)) return SaveStore::kQuickSlot;
        return col == 0 && row < int(order.size()) ? order[size_t(row)] : 0;
    }
    return spread * 6 + col * 3 + row + 1;
}

void MenuGump::hover(int lx, int ly) {
    hover_ = -1;
    hover_extra_ = -1;
    if (mode_ == kOptions) {
        option_hover_ = option_at(lx, ly);
        if (option_hover_ >= 0) option_sel_ = option_hover_;
        return;
    }
    if (mode_ != kMenu) return;
    int it = menu_item_at(lx, ly);
    if (it >= 0) { sel_ = it; sel_mouse_ = true; }
    else if (sel_mouse_) { sel_ = default_item(); sel_mouse_ = false; }
    hover_ = sel_ >= 0 && sel_ < 8 ? sel_ : -1;
    hover_extra_ = sel_ == kItemMap ? 0 : sel_ == kItemHelp ? 1 : -1;
}

bool MenuGump::press(int button, int lx, int ly) {
    down_button_ = 0;
    down_item_ = -1;
    if (mode_ == kMenu) {
        // A menu choice acts when the button comes up over the same entry (at once, not after
        // the double-click time).
        if (button == 1) down_item_ = menu_item_at(lx, ly);
        return true;
    }
    if (mode_ == kOptions) return options_press(button, lx, ly);
    if (mode_ == kLoad || mode_ == kSave) {
        if (editing && edit_touched) return true;  // typing: finish (Enter) or cancel (Esc) first
        if (spread > min_spread() && dogear_hit(0, lx, ly)) down_button_ = 1;
        else if (spread < kSpreads - 1 && dogear_hit(1, lx, ly)) down_button_ = 2;
    }
    return true;
}

void MenuGump::release(int button, int lx, int ly) {
    if (mode_ == kOptions) { options_release(button, lx, ly); return; }
    if (mode_ == kMenu) {
        int it = down_item_;
        down_item_ = -1;
        if (button == 1 && it >= 0 && menu_item_at(lx, ly) == it) {
            sel_ = it;
            mode_tick_ = caret_ticks_;  // the second press of a double-click must not act again
            activate(it);
        }
        return;
    }
    if (down_button_ == 1 && dogear_hit(0, lx, ly)) turn_diary(-1);
    if (down_button_ == 2 && dogear_hit(1, lx, ly)) turn_diary(1);
    if (down_button_) { down_button_ = 0; return; }
    // Diary entries act at once (click or double-click, gumps.md 2.4). An entry only offered
    // for writing (nothing typed yet) gives way to the one clicked.
    if ((mode_ == kLoad || mode_ == kSave) && !(editing && edit_touched) && button == 1)
        if (int slot = slot_at(lx, ly); slot && slot != editing) {
            editing = 0;
            if (mode_ == kSave && slot > 1) diary_sel = slot;
            pick_slot(slot);
        }
}

void MenuGump::click(int button, int lx, int ly) {
    // Menu entries act on release (MenuGump::release); diary entries and the options page too.
}

void MenuGump::double_click(int button, int lx, int ly) {
    // The first click of the pair already chose something (perhaps changing the page): the
    // second press is only that click's twin.
    if (caret_ticks_ - mode_tick_ <= 15) return;
    if (mode_ == kMenu) {
        if (menu_item_at(lx, ly) >= 0) return;  // entries act on release
        if (!(w_.avatar() && w_.avatar()->is(Actor::kDead))) close();
        return;
    }
    if ((mode_ == kLoad || mode_ == kSave) && !(editing && edit_touched) && slot_at(lx, ly) == 0 && !down_button_) set_mode(kMenu);
}

void MenuGump::choose(int entry) {
    PG_LOG("menu: entry %d", entry);
    switch (entry) {
    case 1: w_.play_movie(World::kMovieIntro, true); break;
    case 2: set_mode(kLoad); break;
    case 3:
        if (w_.can_save(this)) set_mode(kSave);
        else PG_LOG("menu: saving is not allowed now");
        break;
    case 4: set_mode(kOptions); break;
    case 5: w_.gumps.add(std::make_unique<CreditsGump>(w_, CreditsGump::kCredits, 30)); break;
    case 6: w_.gumps.add(std::make_unique<QuitGump>(w_)); break;
    case 7:
        if (entry_visible(6)) w_.gumps.add(std::make_unique<CreditsGump>(w_, CreditsGump::kQuotes, 30));
        break;
    case 8:
        if (entry_visible(7)) w_.play_movie(World::kMovieEndgame, true);
        break;
    default: break;
    }
}

void MenuGump::pick_slot(int slot) {
    if (mode_ == kLoad) {
        if (slot == 1) { w_.request = {World::Request::kNewGame, 1, ""}; return; }
        if (!w_.saves || !w_.saves->exists(slot)) return;
        w_.request = {World::Request::kLoad, slot, ""};
        return;
    }
    if (mode_ == kSave && slot > 1) {
        editing = slot;
        diary_sel = slot;
        edit_text = w_.saves ? w_.saves->description(slot) : "";
        edit_touched = false;
        caret_ticks_ = 0;
    }
}

void MenuGump::finish_edit() {
    if (!editing) return;
    if (edit_text.empty()) return;  // empty text is ignored
    w_.request = {World::Request::kSave, editing, edit_text};
    editing = 0;
    close();
}

bool MenuGump::key(int key, unsigned mods) {
    bool dead = w_.avatar() && w_.avatar()->is(Actor::kDead);
    // An entry offered for writing with nothing typed yet: Enter writes it, Backspace starts
    // editing its text, anything else (arrows, page turns, Esc) navigates as if it were not there.
    if (mode_ == kSave && editing && !edit_touched) {
        if (key == kKeyBackspace || key == kKeyDelete) edit_touched = true;
        else if (key != kKeyReturn) editing = 0;
    }
    if (mode_ == kName || editing) {
        std::string& t = edit_text;
        if (key == kKeyBackspace || key == kKeyDelete) { if (!t.empty()) t.pop_back(); edit_touched = true; }
        else if (key == kKeyReturn) {
            if (mode_ == kName) {
                if (!t.empty()) {
                    w_.avatar_name = t;
                    if (Actor* a = w_.avatar()) a->name = t;
                    close();
                }
            } else {
                finish_edit();
            }
        } else if (key == kKeyEscape && editing) {
            editing = 0;  // cancel: the old text is kept
            edit_touched = false;
        }
        return true;
    }
    if (key == kKeyEscape) {
        if (mode_ != kMenu) set_mode(kMenu);
        else if (!dead) close();
        return true;
    }
    if (mode_ == kMenu && key >= '1' && key <= '8') { choose(key - '0'); return true; }
    if (mode_ == kMenu && (key == kKeyUp || key == kKeyDown || key == kKeyLeft || key == kKeyRight ||
                           key == kKeyHome || key == kKeyEnd)) {
        std::vector<int> items = menu_items();
        if (items.empty()) return true;
        int n = int(items.size());
        int at = int(std::find(items.begin(), items.end(), sel_) - items.begin());
        bool up = key == kKeyUp || key == kKeyLeft;
        if (key == kKeyHome) at = 0;
        else if (key == kKeyEnd) at = n - 1;
        else if (at >= n) at = up ? n - 1 : 0;
        else at = (at + (up ? n - 1 : 1)) % n;
        select_item(items[size_t(at)]);
        return true;
    }
    if (mode_ == kMenu && (key == kKeyReturn || key == kKeySpace)) {
        std::vector<int> items = menu_items();
        if (std::find(items.begin(), items.end(), sel_) != items.end()) activate(sel_);
        else if (!items.empty()) select_item(items.front());  // nothing highlighted yet
        return true;
    }
    if (mode_ == kMenu && start_menu && key == 'n') { activate(kItemNewGame); return true; }
    if (mode_ == kMenu && (key == 'm' || key == kKeyTab) && w_.open_map_ui && !dead) { choose_extra(0); return true; }
    if (mode_ == kMenu && key == kKeyF1 && w_.open_help_ui) { choose_extra(1); return true; }
    if (mode_ == kOptions) return options_key(key);
    if (mode_ == kLoad || mode_ == kSave) {
        std::vector<int> slots = spread_slots();
        int at = int(std::find(slots.begin(), slots.end(), diary_sel) - slots.begin());
        int n = int(slots.size());
        if (key == kKeyLeft || key == kKeyRight || key == kKeyPageUp || key == kKeyPageDown) {
            turn_diary(key == kKeyLeft || key == kKeyPageUp ? -1 : 1);
        } else if ((key == kKeyUp || key == kKeyDown) && n > 0) {
            if (at >= n) at = key == kKeyUp ? n - 1 : 0;
            else at = (at + (key == kKeyUp ? n - 1 : 1)) % n;
            diary_sel = slots[size_t(at)];
        } else if ((key == kKeyReturn || key == kKeySpace) && n > 0) {
            if (at < n) pick_slot(diary_sel);
            else diary_sel = slots.front();  // nothing highlighted yet
        }
    }
    return true;
}

bool MenuGump::text(const std::string& utf8) {
    // Write Diary: typing writes into the highlighted entry. Not the text of the key that
    // opened the page (the "3" of the menu arrives just after the page changed).
    if (mode_ == kSave && !editing && ticks_ != mode_ticks_ && diary_sel > 1) pick_slot(diary_sel);
    if (mode_ == kSave && editing && ticks_ == mode_ticks_) return true;
    if (mode_ != kName && !editing) return false;
    if (editing) edit_touched = true;
    for (unsigned char c : utf8) {
        if (c < 32 || c >= 127) continue;  // the game code page: plain ASCII is safe
        const ShapeFont& f = w_.font(mode_ == kName ? 6 : 4);
        std::string next = edit_text + char(c);
        if (mode_ == kName) {
            if (next.size() > 15 || f.text_width(next) > 110) continue;
        } else if (next.size() > 60 || layout_text(f, next, 95, 24).size() > 1) {
            continue;
        }
        edit_text = next;
    }
    return true;
}

}  // namespace pg
