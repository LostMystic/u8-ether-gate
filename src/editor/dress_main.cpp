// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
// pagan_dress: dresses a painted area (docs/area_format.md) with the tile kit.
//   pagan_dress area.json out.json [--seed N] [--png preview.png] [--roofs] [--kit data/tilekit.json]
//               [--game DIR] [--quiet]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

#include "data/gamedata.h"
#include "editor/area_render.h"
#include "editor/dresser.h"

namespace pg {
bool save_png(const std::string& path, const uint32_t* rgba, int w, int h, int scale);
}

namespace {
bool read_file(const std::string& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}
// data/tilekit.json next to the working directory or the executable (build*/bin/ -> engine/data).
std::string find_kit(const char* argv0) {
    if (exists("data/tilekit.json")) return "data/tilekit.json";
    std::string exe = argv0 ? argv0 : "";
    size_t s = exe.find_last_of('/');
    std::string dir = s == std::string::npos ? "." : exe.substr(0, s);
    for (const char* rel : {"/../../data/tilekit.json", "/../data/tilekit.json", "/data/tilekit.json"})
        if (exists(dir + rel)) return dir + rel;
    return "data/tilekit.json";
}
}  // namespace

int main(int argc, char** argv) {
    std::string in, out, png, kit_path, game;
    uint32_t seed = 0;
    bool quiet = false, roofs_png = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--seed" && (v = next())) seed = uint32_t(std::strtoul(v, nullptr, 10));
        else if (a == "--png" && (v = next())) png = v;
        else if (a == "--kit" && (v = next())) kit_path = v;
        else if (a == "--game" && (v = next())) game = v;
        else if (a == "--roofs") roofs_png = true;
        else if (a == "--quiet") quiet = true;
        else if (!a.empty() && a[0] != '-' && in.empty()) in = a;
        else if (!a.empty() && a[0] != '-' && out.empty()) out = a;
        else {
            std::fprintf(stderr,
                         "usage: pagan_dress area.json out.json [--seed N] [--png preview.png] [--roofs] [--kit FILE] [--game DIR] [--quiet]\n");
            return 2;
        }
    }
    if (in.empty() || out.empty()) {
        std::fprintf(stderr, "usage: pagan_dress area.json out.json [--seed N] [--png preview.png] [--roofs] [--kit FILE] [--game DIR]\n");
        return 2;
    }
    if (kit_path.empty()) kit_path = find_kit(argv[0]);
    pg::TileKit kit;
    std::string err, text;
    if (!pg::load_tilekit(kit_path, kit, err)) {
        std::fprintf(stderr, "pagan_dress: %s\n", err.c_str());
        return 1;
    }
    pg::PaintedArea area;
    if (!read_file(in, text) || !pg::parse_area(text, area, err)) {
        std::fprintf(stderr, "pagan_dress: %s: %s\n", in.c_str(), err.empty() ? "cannot read" : err.c_str());
        return 1;
    }
    pg::DressResult r = pg::dress(area, kit, seed);
    std::ofstream f(out, std::ios::binary);
    f << pg::dressed_to_json(area, r);
    if (!f) {
        std::fprintf(stderr, "pagan_dress: cannot write %s\n", out.c_str());
        return 1;
    }
    f.close();
    const pg::Validation& v = r.validation;
    if (!quiet) {
        std::printf("%s: %dx%d cells, theme %s, seed %u: %zu items\n", in.c_str(), area.w, area.h, area.theme.c_str(), r.seed,
                    r.items.size());
        for (const pg::PoiResult& p : r.pois)
            std::printf("  POI %-6s \"%s\" -> %s %s (score %.1f)\n", p.id.c_str(), p.note.c_str(), p.placed ? p.prefab.c_str() : "-",
                        p.label.c_str(), p.score);
        std::printf("  validation: %s; floors %d/%d, wall gaps %d/%d, overlaps %d, unsupported %d, reachable %d/%d positions, "
                    "unreachable POIs %zu\n",
                    v.ok ? "OK" : "PROBLEMS", v.floor_cells - v.uncovered_cells, v.floor_cells, v.wall_gaps, v.wall_links,
                    v.overlaps, v.unsupported, v.reachable, v.positions, v.unreachable_pois.size());
        for (const std::string& p : v.problems) std::printf("    %s\n", p.c_str());
        for (const std::string& w : r.warnings) std::printf("  warning: %s\n", w.c_str());
    }
    if (!png.empty()) {
        if (game.empty()) game = pg::GameFS::locate(argv[0]);
        pg::GameFS fs;
        pg::GameData data;
        if (game.empty() || !fs.open(game) || !data.load(fs)) {
            std::fprintf(stderr, "pagan_dress: --png needs the game data (--game DIR): %s\n", data.error().c_str());
            return 1;
        }
        pg::AreaImage img = pg::render_items(data, pg::to_records(r, roofs_png));
        if (!pg::save_png(png, img.rgba.data(), img.w, img.h, 1)) {
            std::fprintf(stderr, "pagan_dress: cannot write %s\n", png.c_str());
            return 1;
        }
        if (!quiet) std::printf("  preview %s (%dx%d)\n", png.c_str(), img.w, img.h);
    }
    return v.ok ? 0 : 3;
}
