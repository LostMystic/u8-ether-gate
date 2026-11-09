// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "app/web_update.h"

#include "app/game_mode.h"
#include "core/config.h"
#include "core/log.h"
#include "game/movie.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace pg {

void web_update_poll(GameMode* game) {
#ifdef __EMSCRIPTEN__
    static bool shown = false;
    static double next = 0;
    if (shown || !game) return;
    double now = emscripten_get_now();
    if (now < next) return;
    next = now + 1000;
    if (!EM_ASM_INT({ return Module.pgUpdateReady ? 1 : 0; })) return;
    // Not over a movie (it has no overlays); wait until it ends.
    if (game->world().gumps.find<MovieGump>()) return;
    shown = true;
    PG_LOG("web: a newer version is on the server");
    game->toast("New update available - choose Quit in the menu to update", 30 * 12);
#else
    (void)game;
#endif
}

void web_exit_page() {
#ifdef __EMSCRIPTEN__
    Config::global().save();
    EM_ASM({ if (Module.pgExit) Module.pgExit(); });
#endif
}

}  // namespace pg
