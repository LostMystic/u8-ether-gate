// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The web build's updates (web/packs.js, web/exit.html, tools/web_pack.py): the page checks the
// server's version.json now and then and sets Module.pgUpdateReady when a newer version is
// there; the game then says so once. Quitting on the web saves the settings and leaves for
// exit.html (the update / patch notes page). Native: no-ops.
#pragma once

namespace pg {

class GameMode;

// Called every frame by the web main loop; looks once a second and shows "New update
// available" once (a 12 s toast in the game's font).
void web_update_poll(GameMode* game);

// The game quit (main menu Quit, quit box Yes): save settings, sync saves, go to exit.html.
void web_exit_page();

}  // namespace pg
