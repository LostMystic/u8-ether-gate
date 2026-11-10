// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Equipment probe (not a test): every equippable item of the game copy - each shape with an equip
// type 1..6 and each of its inventory frames - with the name its own look usecode says, and its
// inventory frame as a PNG. Used to write data/items/equipment.json (NOTES.md "Companion sheets").
//   pagan_equip_probe GAME_DIR OUT_DIR
#include <cstdio>
#include <string>
#include <sys/stat.h>

#include "app/game_mode.h"
#include "core/config.h"
#include "core/gamefs.h"
#include "data/gamedata.h"
#include "game/world.h"
#include "gfx/surface.h"
#include "platform/platform.h"

using namespace pg;

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { std::printf("usage: pagan_equip_probe GAME_DIR OUT_DIR\n"); return 2; }
    std::string out = argv[2];
    mkdir(out.c_str(), 0755);
    GameFS fs;
    if (!fs.open(argv[1])) { std::printf("game folder not found\n"); return 1; }
    GameData d;
    if (!d.load(fs)) { std::printf("load: %s\n", d.error().c_str()); return 1; }
    GameMode gm(d);
    World& w = gm.world();
    w.skip_opening = true;
    w.ask_name = false;
    w.auto_ui = false;
    gm.start();
    Surface8 surf(320, 200);
    for (int i = 0; i < 60; ++i) gm.tick();
    uint16_t pack = w.equipped(1, 7);
    for (unsigned s = 0; s < d.shapes.count(); ++s) {
        unsigned eq = d.typeflags.get(s).equip_type();
        if (eq < 1 || eq > 6 || (d.typeflags.get(s).raw[0] & 1)) continue;
        const Shape* sh = d.shapes.shape(s);
        if (!sh) continue;
        for (unsigned f = 0; f < sh->frame_count(); f += 2) {
            uint16_t id = w.create_item(s, f);
            if (!id) continue;
            w.move_to_container(id, pack);
            w.barks.clear();
            w.call_event(id, kEvLook);
            std::string text;
            for (int t = 0; t < 30 && text.empty(); ++t) {
                gm.tick();
                for (const Bark& b : w.barks)
                    if (b.speaker == id)
                        for (const TextPage& p : b.pages)
                            for (const std::string& l : p.lines) text += (text.empty() ? "" : " ") + l;
            }
            w.barks.clear();
            const ShapeFrame* fr = d.shapes.frame(s, f);
            std::printf("%u\t%u\t%u\t%dx%d\t%s\n", s, f, eq, fr ? fr->w : 0, fr ? fr->h : 0, text.c_str());
            if (fr) {
                std::vector<uint32_t> px(size_t(fr->w) * fr->h, 0);
                for (size_t k = 0; k < px.size(); ++k) {
                    uint8_t c = fr->pixels[k];
                    if (c == ShapeFrame::kTransparent) continue;
                    auto& rgb = d.palette.rgb8[c];
                    px[k] = 0xFF000000u | uint32_t(rgb[0]) << 16 | uint32_t(rgb[1]) << 8 | rgb[2];
                }
                save_png(out + "/" + std::to_string(s) + "_" + std::to_string(f) + ".png", px.data(), fr->w, fr->h, 4);
            }
        }
    }
    return 0;
}
