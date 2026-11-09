// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
// pagan_tilekit: mines the map dresser's tile kit from the original maps.
//   pagan_tilekit [--game DIR] [--out data/tilekit.json] [--report report.txt] [--quiet]
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "data/gamedata.h"
#include "editor/tilekit_miner.h"

int main(int argc, char** argv) {
    std::string game, out = "data/tilekit.json", report_path;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) game = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--report" && i + 1 < argc) report_path = argv[++i];
        else if (a == "--quiet") quiet = true;
        else {
            std::fprintf(stderr, "usage: pagan_tilekit [--game DIR] [--out data/tilekit.json] [--report FILE] [--quiet]\n");
            return 2;
        }
    }
    if (game.empty()) game = pg::GameFS::locate(argv[0]);
    pg::GameFS fs;
    pg::GameData data;
    if (game.empty() || !fs.open(game) || !data.load(fs)) {
        std::fprintf(stderr, "pagan_tilekit: cannot load the game data from '%s' %s\n", game.c_str(), data.error().c_str());
        return 1;
    }
    pg::TileKit kit;
    std::string report;
    if (!pg::mine_tilekit(data, kit, report)) {
        std::fprintf(stderr, "pagan_tilekit: mining failed\n");
        return 1;
    }
    std::ofstream f(out, std::ios::binary);
    f << pg::tilekit_to_json(kit);
    if (!f) {
        std::fprintf(stderr, "pagan_tilekit: cannot write %s\n", out.c_str());
        return 1;
    }
    if (!report_path.empty()) std::ofstream(report_path) << report;
    if (!quiet) std::fputs(report.c_str(), stdout);
    std::printf("wrote %s\n", out.c_str());
    return 0;
}
