// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "editor/area_model.h"
#include "world/world_layout.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <deque>
#include <set>
#include <sstream>

#include "json/json.hpp"

namespace pg {

using nlohmann::json;

namespace {
int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}  // namespace

namespace cell {
bool valid(char c) {
    switch (c) {
    case kVoid: case kFloor: case kWall: case kDoor: case kPassage: case kWater: case kStairs: case kChasm: return true;
    default: return false;
    }
}
const char* name(char c) {
    switch (c) {
    case kVoid: return "void";
    case kFloor: return "floor";
    case kWall: return "wall";
    case kDoor: return "entrance";
    case kPassage: return "passage";
    case kWater: return "water";
    case kStairs: return "stairs";
    case kChasm: return "chasm";
    default: return "?";
    }
}
}  // namespace cell

char height_digit(int h) {
    h = std::clamp(h, 0, 35);
    return char(h < 10 ? '0' + h : 'a' + (h - 10));
}
int height_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 0;
}

// --- EditArea ----------------------------------------------------------------------------

const AreaCell* EditArea::at(int cx, int cy) const {
    auto it = cells.find({cy, cx});
    return it == cells.end() ? nullptr : &it->second;
}

void EditArea::set(int cx, int cy, const AreaCell& c) {
    if (c.kind == cell::kVoid) cells.erase({cy, cx});
    else cells[{cy, cx}] = c;
}

bool EditArea::bounds(int& x0, int& y0, int& x1, int& y1) const {
    x0 = y0 = INT_MAX;
    x1 = y1 = INT_MIN;
    for (const auto& [k, c] : cells) {
        x0 = std::min(x0, k.second); x1 = std::max(x1, k.second);
        y0 = std::min(y0, k.first); y1 = std::max(y1, k.first);
    }
    for (const AreaPoi& p : pois) {
        x0 = std::min(x0, p.x); x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
    }
    return x0 <= x1;
}

void EditArea::normalize() {
    int x0, y0, x1, y1;
    if (!bounds(x0, y0, x1, y1) || (x0 == 0 && y0 == 0)) return;
    std::map<std::pair<int, int>, AreaCell> moved;
    for (auto& [k, c] : cells) moved[{k.first - y0, k.second - x0}] = c;
    cells.swap(moved);
    for (AreaPoi& p : pois) { p.x -= x0; p.y -= y0; }
    origin[0] += x0 * cell;
    origin[1] += y0 * cell;
}

void EditArea::cell_world(int cx, int cy, int& x, int& y, int& z) const {
    x = origin[0] + cx * cell;
    y = origin[1] + cy * cell;
    const AreaCell* c = at(cx, cy);
    z = origin[2] + base_z + (c ? c->height : 0) * level_z;
}

void EditArea::world_cell(int wx, int wy, int& cx, int& cy) const {
    cx = floor_div(wx - origin[0], cell);
    cy = floor_div(wy - origin[1], cell);
}

int EditArea::poi_index(int cx, int cy) const {
    for (size_t i = 0; i < pois.size(); ++i)
        if (pois[i].x == cx && pois[i].y == cy) return int(i);
    return -1;
}

std::string EditArea::next_poi_id() const {
    for (int n = 1;; ++n) {
        std::string id = "p" + std::to_string(n);
        bool used = false;
        for (const AreaPoi& p : pois) used |= p.id == id;
        if (!used) return id;
    }
}

bool EditArea::operator==(const EditArea& o) const {
    return id == o.id && name == o.name && theme == o.theme && seed == o.seed && cell == o.cell && base_z == o.base_z &&
           level_z == o.level_z && origin[0] == o.origin[0] && origin[1] == o.origin[1] && origin[2] == o.origin[2] &&
           layer == o.layer && cells == o.cells && pois == o.pois;
}

std::string EditArea::to_json() const {
    EditArea a = *this;
    a.normalize();
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    a.bounds(x0, y0, x1, y1);
    // Rows: one string per cell y, trailing void trimmed. Heights / themes only when used.
    std::vector<std::string> rows, heights, themes;
    std::map<std::string, char> legend;  // theme -> key
    bool any_h = false, any_t = false;
    const std::string keys = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int y = 0; y <= y1; ++y) {
        std::string r, h, t;
        for (int x = 0; x <= x1; ++x) {
            const AreaCell* c = a.at(x, y);
            r += c ? c->kind : cell::kVoid;
            h += height_digit(c ? c->height : 0);
            char tk = '.';
            if (c && !c->theme.empty() && c->theme != a.theme) {
                auto it = legend.find(c->theme);
                if (it == legend.end() && legend.size() < keys.size()) it = legend.emplace(c->theme, keys[legend.size()]).first;
                if (it != legend.end()) tk = it->second;
                any_t = true;
            }
            t += tk;
            if (c && c->height) any_h = true;
        }
        size_t n = r.find_last_not_of(' ');
        n = n == std::string::npos ? 0 : n + 1;
        rows.push_back(r.substr(0, n));
        heights.push_back(h.substr(0, n));
        themes.push_back(t.substr(0, n));
    }
    std::ostringstream o;
    o << "{\n  \"format\": \"pagan-area\",\n  \"version\": 1,\n";
    o << "  \"id\": " << json(a.id).dump() << ",\n";
    o << "  \"name\": " << json(a.name).dump() << ",\n";
    o << "  \"theme\": " << json(a.theme).dump() << ",\n";
    o << "  \"seed\": " << a.seed << ",\n  \"cell\": " << a.cell << ",\n  \"base_z\": " << a.base_z
      << ",\n  \"level_z\": " << a.level_z << ",\n";
    o << "  \"origin\": [" << a.origin[0] << ", " << a.origin[1] << ", " << a.origin[2] << "],\n";
    o << "  \"layer\": " << a.layer << ",\n  \"revision\": " << a.revision << ",\n";
    auto rowlist = [&](const char* key, const std::vector<std::string>& v) {
        o << "  \"" << key << "\": [";
        for (size_t i = 0; i < v.size(); ++i) o << (i ? ",\n    " : "\n    ") << json(v[i]).dump();
        o << (v.empty() ? "]" : "\n  ]");
    };
    rowlist("rows", rows);
    if (any_h) { o << ",\n"; rowlist("heights", heights); }
    if (any_t) {
        o << ",\n";
        rowlist("themes", themes);
        json lg = json::object();
        for (const auto& [name, k] : legend) lg[std::string(1, k)] = name;
        o << ",\n  \"theme_legend\": " << lg.dump();
    }
    o << ",\n  \"pois\": [";
    for (size_t i = 0; i < a.pois.size(); ++i) {
        const AreaPoi& p = a.pois[i];
        json pj = {{"id", p.id}, {"x", p.x}, {"y", p.y}, {"note", p.note}, {"tags", p.tags}};
        if (!p.prefab.empty()) pj["prefab"] = p.prefab;
        if (p.radius != 2) pj["radius"] = p.radius;
        o << (i ? ",\n    " : "\n    ") << pj.dump();
    }
    o << (a.pois.empty() ? "]" : "\n  ]") << "\n}\n";
    return o.str();
}

bool EditArea::from_json(const std::string& text, std::string& err) {
    json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) { err = "not a JSON object"; return false; }
    if (j.value("format", "") != "pagan-area") { err = "format is not \"pagan-area\""; return false; }
    if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() < 1) {
        err = "bad version"; return false;
    }
    if (!j.contains("rows") || !j["rows"].is_array()) { err = "no \"rows\" array"; return false; }
    EditArea a;
    try {
        a.id = j.value("id", "");
        a.name = j.value("name", "");
        a.theme = j.value("theme", "cave");
        a.seed = j.value("seed", 1u);
        a.cell = j.value("cell", 128);
        a.base_z = j.value("base_z", 0);
        a.level_z = j.value("level_z", 8);
        a.layer = j.value("layer", 0);
        a.revision = j.value("revision", 0);
        if (a.cell <= 0) { err = "bad cell size"; return false; }
        if (j.contains("origin")) {
            const json& o = j["origin"];
            if (!o.is_array() || o.size() < 2 || o.size() > 3) { err = "origin must be [x, y, z]"; return false; }
            for (size_t k = 0; k < o.size(); ++k) a.origin[k] = o[k].get<int>();
        }
        auto rows_of = [&](const char* key) {
            std::vector<std::string> v;
            if (j.contains(key) && j[key].is_array())
                for (const json& r : j[key]) v.push_back(r.is_string() ? r.get<std::string>() : std::string());
            return v;
        };
        std::vector<std::string> rows = rows_of("rows"), hs = rows_of("heights"), ts = rows_of("themes");
        std::map<char, std::string> legend;
        if (j.contains("theme_legend") && j["theme_legend"].is_object())
            for (auto& [k, v] : j["theme_legend"].items())
                if (!k.empty() && v.is_string()) legend[k[0]] = v.get<std::string>();
        for (size_t y = 0; y < rows.size(); ++y)
            for (size_t x = 0; x < rows[y].size(); ++x) {
                char k = rows[y][x];
                if (k == cell::kVoid) continue;
                AreaCell c;
                c.kind = cell::valid(k) ? k : cell::kFloor;
                if (y < hs.size() && x < hs[y].size()) c.height = uint8_t(height_value(hs[y][x]));
                if (y < ts.size() && x < ts[y].size()) {
                    auto it = legend.find(ts[y][x]);
                    if (it != legend.end()) c.theme = it->second;
                }
                a.cells[{int(y), int(x)}] = c;
            }
        if (j.contains("pois") && j["pois"].is_array())
            for (const json& p : j["pois"]) {
                if (!p.is_object() || !p.contains("x") || !p.contains("y")) { err = "a POI has no x / y"; return false; }
                AreaPoi q;
                q.x = p["x"].get<int>();
                q.y = p["y"].get<int>();
                q.id = p.value("id", "");
                q.note = p.value("note", "");
                q.prefab = p.value("prefab", "");
                q.radius = p.value("radius", 2);
                if (p.contains("tags") && p["tags"].is_array())
                    for (const json& t : p["tags"])
                        if (t.is_string()) q.tags.push_back(t.get<std::string>());
                if (q.id.empty()) q.id = "poi" + std::to_string(a.pois.size() + 1);
                a.pois.push_back(q);
            }
    } catch (const std::exception& e) {
        err = std::string("bad field: ") + e.what();
        return false;
    }
    *this = std::move(a);
    return true;
}

// --- Brushes -----------------------------------------------------------------------------

std::vector<std::pair<int, int>> brush_cells(BrushShape shape, int size, int cx, int cy) {
    std::vector<std::pair<int, int>> out;
    size = std::clamp(size, 1, 64);
    // A size-n brush covers n cells across; even sizes extend one more to the +x / +y side.
    int lo = -(size - 1) / 2, hi = size / 2;
    double r = size / 2.0;
    double c = (size % 2) ? 0.0 : 0.5;  // centre offset for even sizes
    for (int dy = lo; dy <= hi; ++dy)
        for (int dx = lo; dx <= hi; ++dx) {
            if (shape == BrushShape::kRound && size > 2) {
                double fx = dx - c, fy = dy - c;
                if (fx * fx + fy * fy > r * r + 0.25) continue;
            }
            out.push_back({cx + dx, cy + dy});
        }
    return out;
}

std::vector<std::pair<int, int>> line_cells(BrushShape shape, int size, int x0, int y0, int x1, int y1) {
    std::set<std::pair<int, int>> seen;
    std::vector<std::pair<int, int>> out;
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int n = 0; n < 100000; ++n) {
        for (auto p : brush_cells(shape, size, x0, y0))
            if (seen.insert(p).second) out.push_back(p);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    return out;
}

std::vector<std::pair<int, int>> rect_cells(int x0, int y0, int x1, int y1, bool outline) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    std::vector<std::pair<int, int>> out;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (!outline || x == x0 || x == x1 || y == y0 || y == y1) out.push_back({x, y});
    return out;
}

namespace {
bool apply_value(EditArea& a, int x, int y, const PaintValue& v) {
    const AreaCell* old = a.at(x, y);
    if (v.kind == cell::kVoid) {
        if (!old) return false;
        a.erase(x, y);
        int pi = a.poi_index(x, y);
        if (pi >= 0) a.pois.erase(a.pois.begin() + pi);  // erasing a cell removes its marker
        return true;
    }
    if (!old && v.kind == 0) return false;  // height / theme only: never creates cells
    AreaCell c = old ? *old : AreaCell{};
    if (v.kind) c.kind = v.kind;
    if (v.height >= 0) c.height = uint8_t(std::clamp(v.height, 0, 35));
    if (v.theme) c.theme = *v.theme == a.theme ? std::string() : *v.theme;
    if (old && *old == c) return false;
    a.set(x, y, c);
    return true;
}
}  // namespace

int paint_cells(EditArea& a, const std::vector<std::pair<int, int>>& cells, const PaintValue& v) {
    int n = 0;
    for (auto [x, y] : cells) n += apply_value(a, x, y, v);
    return n;
}

int flood_fill(EditArea& a, int cx, int cy, const PaintValue& v, int margin) {
    int x0, y0, x1, y1;
    if (!a.bounds(x0, y0, x1, y1)) { x0 = x1 = cx; y0 = y1 = cy; }
    x0 = std::min(x0 - margin, cx); y0 = std::min(y0 - margin, cy);
    x1 = std::max(x1 + margin, cx); y1 = std::max(y1 + margin, cy);
    const AreaCell* s = a.at(cx, cy);
    const char sk = s ? s->kind : cell::kVoid;
    const int sh = s ? s->height : 0;
    auto same = [&](int x, int y) {
        const AreaCell* c = a.at(x, y);
        return (c ? c->kind : cell::kVoid) == sk && (c ? c->height : 0) == sh;
    };
    std::set<std::pair<int, int>> seen;
    std::deque<std::pair<int, int>> q{{cx, cy}};
    std::vector<std::pair<int, int>> region;
    seen.insert({cx, cy});
    while (!q.empty()) {
        auto [x, y] = q.front();
        q.pop_front();
        region.push_back({x, y});
        const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (auto& e : d) {
            int nx = x + e[0], ny = y + e[1];
            if (nx < x0 || ny < y0 || nx > x1 || ny > y1) continue;
            if (!seen.insert({nx, ny}).second || !same(nx, ny)) continue;
            q.push_back({nx, ny});
        }
    }
    return paint_cells(a, region, v);
}

// --- WorldLayout -------------------------------------------------------------------------

std::string WorldLayout::to_json() const {
    // The atlas layout's text with the areas (and the kept fields) added before the end.
    std::string base = atlas.to_json();
    while (!base.empty() && (base.back() == '\n' || base.back() == ' ')) base.pop_back();
    if (!base.empty() && base.back() == '}') base.pop_back();
    while (!base.empty() && (base.back() == '\n' || base.back() == ' ')) base.pop_back();
    std::ostringstream o;
    o << base;
    o << ",\n  \"format\": \"pagan-world-layout\",\n  \"version\": 1,\n  \"revision\": " << revision << ",\n  \"areas\": [";
    for (size_t i = 0; i < areas.size(); ++i) {
        const LayoutArea& a = areas[i];
        o << (i ? ",\n    " : "\n    ") << "{\"id\": " << json(a.id).dump() << ", \"name\": " << json(a.name).dump()
          << ", \"file\": " << json("areas/" + a.id + ".json").dump() << ", \"offset\": [" << a.offset[0] << ", "
          << a.offset[1] << ", " << a.offset[2] << "], \"layer\": " << a.layer << "}";
    }
    o << (areas.empty() ? "]" : "\n  ]");
    if (!extra.empty()) {
        json x = json::parse(extra, nullptr, false);
        if (x.is_object())
            for (auto& [k, v] : x.items()) {
                o << ",\n  " << json(k).dump() << ": ";
                if (v.is_array() && !v.empty()) {  // one element per line (the stitched world's seams and links)
                    o << "[";
                    for (size_t i = 0; i < v.size(); ++i) o << (i ? ",\n    " : "\n    ") << v[i].dump();
                    o << "\n  ]";
                } else {
                    o << v.dump();
                }
            }
    }
    o << "\n}\n";
    return o.str();
}

bool WorldLayout::from_json(const std::string& text, std::string& err) {
    WorldLayout w;
    if (!w.atlas.from_json(text, err)) return false;
    json j = json::parse(text, nullptr, false, true);
    w.revision = j.value("revision", 0);
    if (j.contains("areas")) {
        if (!j["areas"].is_array()) { err = "\"areas\" is not an array"; return false; }
        for (const json& a : j["areas"]) {
            if (!a.is_object() || !a.contains("id") || !a["id"].is_string()) { err = "an area entry has no id"; return false; }
            LayoutArea la;
            la.id = a["id"].get<std::string>();
            la.name = a.value("name", la.id);
            la.layer = a.value("layer", 0);
            if (a.contains("offset") && a["offset"].is_array())
                for (size_t k = 0; k < 3 && k < a["offset"].size(); ++k)
                    if (a["offset"][k].is_number_integer()) la.offset[k] = a["offset"][k].get<int>();
            w.areas.push_back(la);
        }
    }
    json x = json::object();
    for (auto& [k, v] : j.items())
        if (k != "comment" && k != "groups" && k != "areas" && k != "format" && k != "version" && k != "revision") x[k] = v;
    if (!x.empty()) w.extra = x.dump();
    *this = std::move(w);
    return true;
}

// --- EditDoc -----------------------------------------------------------------------------

int EditDoc::find(const std::string& id) const {
    for (size_t i = 0; i < areas.size(); ++i)
        if (areas[i].id == id) return int(i);
    return -1;
}

std::string EditDoc::unique_id(const std::string& name) const {
    std::string base;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) base += char(std::tolower(static_cast<unsigned char>(c)));
        else if ((c == ' ' || c == '-' || c == '_') && !base.empty() && base.back() != '_') base += '_';
        if (base.size() >= 32) break;
    }
    while (!base.empty() && base.back() == '_') base.pop_back();
    if (base.empty()) base = "area";
    std::string id = base;
    for (int n = 2; find(id) >= 0; ++n) id = base + "_" + std::to_string(n);
    return id;
}

void EditDoc::begin(const std::string& label) {
    if (open_) commit();
    cur_ = Change{};
    cur_.label = label;
    open_ = true;
}

void EditDoc::touch_area(const std::string& id) {
    if (!open_) begin("edit");
    for (const Snap& s : cur_.snaps)
        if (s.group < 0 && s.id == id) return;
    Snap s;
    s.id = id;
    int i = find(id);
    if (i >= 0) s.before = areas[size_t(i)];
    cur_.snaps.push_back(std::move(s));
}

void EditDoc::touch_group(int group) {
    if (!open_) begin("edit");
    if (group < 0 || size_t(group) >= layout.atlas.groups.size()) return;
    for (const Snap& s : cur_.snaps)
        if (s.group == group) return;
    Snap s;
    s.group = group;
    s.gbefore = layout.atlas.groups[size_t(group)];
    cur_.snaps.push_back(std::move(s));
}

void EditDoc::touch_layout_extra() {
    if (!open_) begin("edit");
    for (const Snap& s : cur_.snaps)
        if (s.group == -2) return;
    Snap s;
    s.group = -2;
    s.xbefore = layout.extra;
    cur_.snaps.push_back(std::move(s));
}

void EditDoc::commit() {
    if (!open_) return;
    open_ = false;
    bool differs = false;
    for (Snap& s : cur_.snaps) {
        if (s.group == -2) {
            s.xafter = layout.extra;
            differs |= *s.xafter != *s.xbefore;
        } else if (s.group >= 0) {
            s.gafter = layout.atlas.groups[size_t(s.group)];
            const AtlasGroup &b = *s.gbefore, &a = *s.gafter;
            differs |= a.dx != b.dx || a.dy != b.dy || a.dz != b.dz || a.layer != b.layer || a.label != b.label;
        } else {
            int i = find(s.id);
            if (i >= 0) s.after = areas[size_t(i)];
            differs |= s.before.has_value() != s.after.has_value() || (s.before && !(*s.before == *s.after));
        }
    }
    if (!differs) return;
    undo_.push_back(std::move(cur_));
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
    ++changes;
}

void EditDoc::cancel() {
    if (!open_) return;
    open_ = false;
    for (auto it = cur_.snaps.rbegin(); it != cur_.snaps.rend(); ++it) {
        if (it->group == -2) layout.extra = *it->xbefore;
        else if (it->group >= 0) layout.atlas.groups[size_t(it->group)] = *it->gbefore;
        else place_area(it->id, it->before);
    }
}

void EditDoc::place_area(const std::string& id, const std::optional<EditArea>& a) {
    int i = find(id);
    if (!a) {
        if (i >= 0) areas.erase(areas.begin() + i);
    } else if (i >= 0) {
        areas[size_t(i)] = *a;
    } else {
        areas.push_back(*a);
    }
}

void EditDoc::apply(const Change& c, bool forward) {
    for (const Snap& s : c.snaps) {
        if (s.group == -2) {
            layout.extra = forward ? *s.xafter : *s.xbefore;
        } else if (s.group >= 0) {
            if (size_t(s.group) < layout.atlas.groups.size()) layout.atlas.groups[size_t(s.group)] = forward ? *s.gafter : *s.gbefore;
        } else {
            place_area(s.id, forward ? s.after : s.before);
        }
    }
    ++changes;
}

bool EditDoc::undo() {
    if (open_) commit();
    if (undo_.empty()) return false;
    Change c = std::move(undo_.back());
    undo_.pop_back();
    apply(c, false);
    redo_.push_back(std::move(c));
    return true;
}

bool EditDoc::redo() {
    if (open_) commit();
    if (redo_.empty()) return false;
    Change c = std::move(redo_.back());
    redo_.pop_back();
    apply(c, true);
    undo_.push_back(std::move(c));
    return true;
}

EditArea& EditDoc::create_area(const std::string& name, const int origin[3], int layer, const std::string& theme) {
    std::string id = unique_id(name);
    begin("new area " + name);
    touch_area(id);
    EditArea a;
    a.id = id;
    a.name = name;
    a.theme = theme;
    std::copy(origin, origin + 3, a.origin);
    a.layer = layer;
    a.seed = 1;
    areas.push_back(a);
    commit();
    return areas[size_t(find(id))];
}

bool EditDoc::delete_area(const std::string& id) {
    if (find(id) < 0) return false;
    begin("delete area");
    touch_area(id);
    areas.erase(areas.begin() + find(id));
    commit();
    return true;
}

namespace {
template <class F>
bool area_change(EditDoc& d, const std::string& id, const std::string& label, F f) {
    int i = d.find(id);
    if (i < 0) return false;
    d.begin(label);
    d.touch_area(id);
    f(d.areas[size_t(i)]);
    d.commit();
    return true;
}
}  // namespace

bool EditDoc::rename_area(const std::string& id, const std::string& name) {
    return area_change(*this, id, "rename", [&](EditArea& a) { a.name = name; });
}
bool EditDoc::move_area(const std::string& id, int dx, int dy, int dz) {
    return area_change(*this, id, "move area", [&](EditArea& a) { a.origin[0] += dx; a.origin[1] += dy; a.origin[2] += dz; });
}
bool EditDoc::set_area_layer(const std::string& id, int layer) {
    return area_change(*this, id, "area layer", [&](EditArea& a) { a.layer = layer; });
}
bool EditDoc::set_area_theme(const std::string& id, const std::string& theme) {
    return area_change(*this, id, "area theme", [&](EditArea& a) { a.theme = theme; });
}
bool EditDoc::set_area_seed(const std::string& id, uint32_t seed) {
    return area_change(*this, id, "seed", [&](EditArea& a) { a.seed = seed; });
}
bool EditDoc::add_poi(const std::string& id, int cx, int cy, const std::string& note, const std::vector<std::string>& tags) {
    return area_change(*this, id, "add marker", [&](EditArea& a) {
        AreaPoi p;
        p.id = a.next_poi_id();
        p.x = cx; p.y = cy; p.note = note; p.tags = tags;
        a.pois.push_back(p);
    });
}
bool EditDoc::edit_poi(const std::string& id, int index, const std::string& note, const std::vector<std::string>& tags) {
    int i = find(id);
    if (i < 0 || index < 0 || size_t(index) >= areas[size_t(i)].pois.size()) return false;
    return area_change(*this, id, "edit marker", [&](EditArea& a) { a.pois[size_t(index)].note = note; a.pois[size_t(index)].tags = tags; });
}
bool EditDoc::delete_poi(const std::string& id, int index) {
    int i = find(id);
    if (i < 0 || index < 0 || size_t(index) >= areas[size_t(i)].pois.size()) return false;
    return area_change(*this, id, "delete marker", [&](EditArea& a) { a.pois.erase(a.pois.begin() + index); });
}

bool EditDoc::move_group(int group, int dx, int dy, int dz) {
    if (group < 0 || size_t(group) >= layout.atlas.groups.size()) return false;
    begin("move map group");
    touch_group(group);
    AtlasGroup& g = layout.atlas.groups[size_t(group)];
    g.dx += dx; g.dy += dy; g.dz += dz;
    if (g.from_data || g.derived.find("moved by hand") == std::string::npos)
        g.derived = "manual: moved by hand in the editor (was: " + g.derived + ")";
    g.from_data = false;
    commit();
    return true;
}

bool EditDoc::set_group_layer(int group, int layer) {
    if (group < 0 || size_t(group) >= layout.atlas.groups.size()) return false;
    begin("map group layer");
    touch_group(group);
    AtlasGroup& g = layout.atlas.groups[size_t(group)];
    // The group's z follows its layer (dz = 256 x layer + its height trim; world_layout.h).
    g.dz += (layer - g.layer) * kLayerZ;
    g.layer = layer;
    commit();
    return true;
}

void EditDoc::sync_layout() {
    layout.areas.clear();
    for (EditArea& a : areas) {
        a.normalize();
        LayoutArea la;
        la.id = a.id;
        la.name = a.name;
        std::copy(a.origin, a.origin + 3, la.offset);
        la.layer = a.layer;
        layout.areas.push_back(la);
    }
}

std::string EditDoc::bundle_json() {
    sync_layout();
    std::ostringstream o;
    o << "{\n\"format\": \"pagan-editor-bundle\",\n\"version\": 1,\n\"layout\": " << layout.to_json() << ",\n\"areas\": [";
    for (size_t i = 0; i < areas.size(); ++i) o << (i ? ",\n" : "\n") << areas[i].to_json();
    o << "]\n}\n";
    return o.str();
}

bool EditDoc::load_bundle(const std::string& text, std::string& err) {
    json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) { err = "not a JSON object"; return false; }
    std::string fmt = j.value("format", "");
    WorldLayout nl = layout;
    std::vector<EditArea> na;
    if (fmt == "pagan-area") {  // a single area file: added (or replaced)
        EditArea a;
        if (!a.from_json(text, err)) return false;
        if (a.id.empty()) a.id = unique_id(a.name.empty() ? "area" : a.name);
        begin("import area");
        touch_area(a.id);
        place_area(a.id, a);
        commit();
        return true;
    }
    if (fmt != "pagan-editor-bundle") { err = "not an editor bundle or an area file"; return false; }
    if (j.contains("layout") && j["layout"].is_object()) {
        if (!nl.from_json(j["layout"].dump(), err)) { err = "layout: " + err; return false; }
    }
    if (j.contains("areas") && j["areas"].is_array())
        for (const json& a : j["areas"]) {
            EditArea e;
            if (!e.from_json(a.dump(), err)) { err = "area: " + err; return false; }
            if (e.id.empty()) e.id = "area" + std::to_string(na.size() + 1);
            na.push_back(std::move(e));
        }
    // The layout's area entries place the areas.
    for (EditArea& e : na)
        for (const LayoutArea& la : nl.areas)
            if (la.id == e.id) { std::copy(la.offset, la.offset + 3, e.origin); e.layer = la.layer; }
    layout = std::move(nl);
    areas = std::move(na);
    clear_history();
    ++changes;
    return true;
}

}  // namespace pg
