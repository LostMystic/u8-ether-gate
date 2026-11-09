# Pagan engine - a clean-room reimplementation of the Ultima 8 engine.
# Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
cmake_minimum_required(VERSION 3.16)
project(Pagan LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# A web build for any static host: packs only our own data; the player chooses their own game
# folder in the page (web/packs.js PGGame, web/shell.html). No game files in the build at all.
option(PAGAN_WEB_PUBLIC "Web: no game data in the build; the page asks for the player's own game folder" OFF)

# A checkout's own settings (not part of a release): see tools/make_release.sh.
if(NOT PAGAN_WEB_PUBLIC)
  include(${CMAKE_SOURCE_DIR}/private.cmake OPTIONAL)
endif()

# The version: the top numbered version of the changelog.
file(STRINGS ${CMAKE_SOURCE_DIR}/CHANGELOG.md PG_CHANGELOG_VERSIONS REGEX "^## [0-9]+\\.[0-9.]+")
list(GET PG_CHANGELOG_VERSIONS 0 PG_TOP_VERSION)
string(REGEX MATCH "[0-9]+\\.[0-9.]*[0-9]" PAGAN_VERSION "${PG_TOP_VERSION}")

# Where the original game data lives (read-only). Used by the tests and the web build.
set(PAGAN_GAME_DIR "${CMAKE_SOURCE_DIR}/../game" CACHE PATH "Original Ultima 8 folder (STATIC/, GAMEDAT/ ...)")
get_filename_component(PAGAN_GAME_DIR "${PAGAN_GAME_DIR}" REALPATH)

# --- SDL2 ---------------------------------------------------------------------
# Web: Emscripten's SDL2 port. Native: a system SDL2 development package if there is one,
# otherwise the vendored headers in third_party/sdl2 linked straight against the SDL2
# runtime library (SDL2's ABI is stable), so no -devel package is needed.
add_library(pg_sdl2 INTERFACE)
if(EMSCRIPTEN)
  target_compile_options(pg_sdl2 INTERFACE -sUSE_SDL=2)
  target_link_options(pg_sdl2 INTERFACE -sUSE_SDL=2)
else()
  find_package(SDL2 QUIET CONFIG)
  if(TARGET SDL2::SDL2)
    message(STATUS "SDL2: system development package")
    target_link_libraries(pg_sdl2 INTERFACE SDL2::SDL2)
  else()
    find_library(SDL2_RUNTIME NAMES SDL2-2.0 SDL2 libSDL2-2.0.so.0
      PATHS /usr/lib64 /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib)
    if(NOT SDL2_RUNTIME)
      message(FATAL_ERROR "SDL2 runtime library not found (install SDL2)")
    endif()
    message(STATUS "SDL2: vendored headers + runtime ${SDL2_RUNTIME}")
    target_include_directories(pg_sdl2 SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/third_party/sdl2)
    target_link_libraries(pg_sdl2 INTERFACE ${SDL2_RUNTIME})
  endif()
endif()

set(PG_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

# --- Core library: data loaders, 8-bit drawing, world (no SDL) ------------------
file(GLOB PG_CORE_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/core/*.cpp ${CMAKE_SOURCE_DIR}/src/data/*.cpp
  ${CMAKE_SOURCE_DIR}/src/gfx/*.cpp ${CMAKE_SOURCE_DIR}/src/world/*.cpp
  ${CMAKE_SOURCE_DIR}/src/kernel/*.cpp ${CMAKE_SOURCE_DIR}/src/usecode/*.cpp ${CMAKE_SOURCE_DIR}/src/game/*.cpp
  ${CMAKE_SOURCE_DIR}/src/audio/*.cpp)
add_library(pagan_core STATIC ${PG_CORE_SOURCES})
target_include_directories(pagan_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(pagan_core SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party)
target_compile_options(pagan_core PRIVATE ${PG_WARNINGS})

# --- Map dresser and tile kit (src/editor, docs/area_format.md; no SDL) ------------
file(GLOB PG_EDITOR_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/editor/*.cpp)
list(FILTER PG_EDITOR_SOURCES EXCLUDE REGEX "_main\\.cpp$")
if(NOT PAGAN_SAVE_SERVICE)
  list(FILTER PG_EDITOR_SOURCES EXCLUDE REGEX "server_storage\\.cpp$")
endif()
add_library(pagan_editor STATIC ${PG_EDITOR_SOURCES})
target_link_libraries(pagan_editor PUBLIC pagan_core)
target_compile_options(pagan_editor PRIVATE ${PG_WARNINGS})

# The game mode and what it draws (also compiled into the native tests).
set(PG_GAMEMODE_SOURCES ${CMAKE_SOURCE_DIR}/src/app/game_mode.cpp ${CMAKE_SOURCE_DIR}/src/app/game_view.cpp
  ${CMAKE_SOURCE_DIR}/src/app/game_pad.cpp ${CMAKE_SOURCE_DIR}/src/app/atlas_view.cpp ${CMAKE_SOURCE_DIR}/src/app/rgba_draw.cpp
  ${CMAKE_SOURCE_DIR}/src/app/debug_tools.cpp ${CMAKE_SOURCE_DIR}/src/platform/screenshot.cpp
  ${CMAKE_SOURCE_DIR}/src/app/turn_ui.cpp ${CMAKE_SOURCE_DIR}/src/app/hud_panel.cpp ${CMAKE_SOURCE_DIR}/src/app/game_hud.cpp
  ${CMAKE_SOURCE_DIR}/src/app/auto_walk.cpp)

# The critical path and map links (specs/data/*.json, written by the spec side) are copied into
# engine/data at configure time, and again whenever a file appears or changes there, so the
# native build finds them in data/ and the web build packs / preloads them with the rest.
file(GLOB PG_SPEC_DATA CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/../specs/data/*.json)
foreach(f ${PG_SPEC_DATA})
  get_filename_component(n ${f} NAME)
  configure_file(${f} ${CMAKE_SOURCE_DIR}/data/${n} COPYONLY)
endforeach()

# --- The engine ------------------------------------------------------------------
file(GLOB PG_APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/platform/*.cpp ${CMAKE_SOURCE_DIR}/src/app/*.cpp)
add_executable(pagan ${PG_APP_SOURCES})
target_link_libraries(pagan PRIVATE pagan_editor pagan_core pg_sdl2 m)
target_compile_options(pagan PRIVATE ${PG_WARNINGS})
target_compile_definitions(pagan PRIVATE PAGAN_VERSION="${PAGAN_VERSION}")

if(EMSCRIPTEN)
  # Output: build-web/web/pagan.{html,js,wasm,data}. The game folder is preloaded into
  # /game of the virtual file system (pagan.data, fetched by pagan.js before main runs).
  set_target_properties(pagan PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/web SUFFIX ".html")
  set(PG_PRELOAD "")
  foreach(dir STATIC GAMEDAT USECODE SOUND SAVEGAME)
    if(EXISTS "${PAGAN_GAME_DIR}/${dir}")
      list(APPEND PG_PRELOAD "SHELL:--preload-file ${PAGAN_GAME_DIR}/${dir}@/game/${dir}")
    endif()
  endforeach()
  if(EXISTS "${CMAKE_SOURCE_DIR}/data/soundfont/u8_gm.sf2")  # the music SoundFont (data/soundfont/README.md)
    list(APPEND PG_PRELOAD "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/data/soundfont/u8_gm.sf2@/data/soundfont/u8_gm.sf2")
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/data/atlas_layout.json")  # the world atlas layout (app/atlas_view.h)
    list(APPEND PG_PRELOAD "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/data/atlas_layout.json@/data/atlas_layout.json")
  endif()
  foreach(n critpath.json map_links.json)  # the critical path (app/debug_tools.h)
    if(EXISTS "${CMAKE_SOURCE_DIR}/data/${n}")
      list(APPEND PG_PRELOAD "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/data/${n}@/data/${n}")
    endif()
  endforeach()
  # Packs (the default): no pagan.data. Every build also runs tools/web_pack.py, which packs ALL
  # of the game folders and ALL of engine/data/ (so a new data file needs no line here) into
  # web/files/*.pack, copies the program to web/files/pagan.<hash>.wasm and writes
  # web/version.json from CHANGELOG.md; web/shell.html loads them through web/packs.js (cached in
  # the browser, only changed packs are downloaded). OFF: the old --preload-file pagan.data.
  option(PAGAN_WEB_PACKS "Web: game data in cached packs instead of a preloaded pagan.data" ON)
  set(PG_PACK_GAME ${PAGAN_GAME_DIR})
  set(PG_PACK_EXTRA "")
  if(PAGAN_WEB_PUBLIC)
    if(NOT PAGAN_WEB_PACKS)
      message(FATAL_ERROR "PAGAN_WEB_PUBLIC needs PAGAN_WEB_PACKS")
    endif()
    set(PG_PACK_GAME "-")  # no game folder: only our own data is packed
    set(PG_PACK_EXTRA COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_BINARY_DIR}/web/LICENSE.txt)
  endif()
  if(PAGAN_WEB_PACKS)
    set(PG_PRELOAD "")
    find_package(Python3 COMPONENTS Interpreter)
    set(PG_PY python3)
    if(Python3_EXECUTABLE)
      set(PG_PY ${Python3_EXECUTABLE})
    endif()
    add_custom_target(pagan_web_packs ALL
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/web/exit.html ${CMAKE_SOURCE_DIR}/web/packs.js ${CMAKE_BINARY_DIR}/web/
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/data/icon/favicon.ico
        ${CMAKE_SOURCE_DIR}/data/icon/pagan_icon_32.png ${CMAKE_SOURCE_DIR}/data/icon/pagan_icon_256.png ${CMAKE_BINARY_DIR}/web/
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/web/pagan.html ${CMAKE_BINARY_DIR}/web/index.html
      COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_BINARY_DIR}/web/pagan.data
      ${PG_PACK_EXTRA}
      COMMAND ${PG_PY} ${CMAKE_SOURCE_DIR}/tools/web_pack.py ${CMAKE_BINARY_DIR}/web ${PG_PACK_GAME}
        ${CMAKE_SOURCE_DIR}/data ${CMAKE_SOURCE_DIR}/CHANGELOG.md
      DEPENDS pagan
      COMMENT "Packing the web data (tools/web_pack.py)")
  endif()
  target_link_options(pagan PRIVATE
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=128MB -sSTACK_SIZE=2MB
    -sEXPORTED_RUNTIME_METHODS=callMain,FS,addRunDependency,removeRunDependency -sFORCE_FILESYSTEM=1 -lidbfs.js
    "SHELL:-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$stringToNewUTF8"
    ${PG_PRELOAD}
    "SHELL:--shell-file ${CMAKE_SOURCE_DIR}/web/shell.html")
  set_property(TARGET pagan APPEND PROPERTY LINK_DEPENDS ${CMAKE_SOURCE_DIR}/web/shell.html)
else()
  set_target_properties(pagan PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

  # --- Tests (native only) --------------------------------------------------------
  enable_testing()
  add_executable(pagan_tests ${CMAKE_SOURCE_DIR}/tests/test_data.cpp ${CMAKE_SOURCE_DIR}/tests/test_usecode.cpp
    ${CMAKE_SOURCE_DIR}/tests/test_world.cpp ${CMAKE_SOURCE_DIR}/tests/test_ui.cpp ${CMAKE_SOURCE_DIR}/tests/test_controls.cpp
    ${CMAKE_SOURCE_DIR}/tests/test_audio.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_tests PRIVATE pagan_core)
  target_compile_options(pagan_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME data COMMAND pagan_tests ${PAGAN_GAME_DIR} ${CMAKE_BINARY_DIR}/test_saves
    ${CMAKE_SOURCE_DIR}/data/soundfont/u8_gm.sf2)
  # The early-game play-test driven through the game mode's input (milestone 4).
  add_executable(pagan_playtest ${CMAKE_SOURCE_DIR}/tests/playtest.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_playtest PRIVATE pagan_core)
  target_compile_options(pagan_playtest PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_playtest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME playtest COMMAND pagan_playtest ${PAGAN_GAME_DIR} ${CMAKE_BINARY_DIR}/playtest_shots ${CMAKE_BINARY_DIR}/playtest_saves)
  # Container sort (game/container_sort.*): merging, grouping, clickable layouts, shots.
  add_executable(pagan_sort_tests ${CMAKE_SOURCE_DIR}/tests/test_sort.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_sort_tests PRIVATE pagan_core)
  target_compile_options(pagan_sort_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_sort_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME sort COMMAND pagan_sort_tests ${PAGAN_GAME_DIR} ${CMAKE_BINARY_DIR}/sort_shots ${CMAKE_BINARY_DIR}/sort_saves)
  # Milestone 6: a virtual controller through SDL's GameController API into the game mode.
  add_executable(pagan_pad_tests ${CMAKE_SOURCE_DIR}/tests/test_pad_sdl.cpp ${PG_GAMEMODE_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/platform/platform.cpp)
  target_link_libraries(pagan_pad_tests PRIVATE pagan_core pg_sdl2 m)
  target_compile_options(pagan_pad_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_pad_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME controller COMMAND ${CMAKE_COMMAND} -E env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
    $<TARGET_FILE:pagan_pad_tests> ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data)
  # Debug menu, critical path and autosaves (app/debug_tools.*, game/critpath.*, game/debug_menu.*).
  add_executable(pagan_debug_tests ${CMAKE_SOURCE_DIR}/tests/test_debug.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_debug_tests PRIVATE pagan_core)
  target_compile_options(pagan_debug_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_debug_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME debug COMMAND pagan_debug_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/tests/data
    ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/debug_shots ${CMAKE_BINARY_DIR}/debug_saves)
  # The critical-path walking route (game/route_guide.*): routes found, walked, sliced, drawn.
  add_executable(pagan_route_tests ${CMAKE_SOURCE_DIR}/tests/test_route.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_route_tests PRIVATE pagan_core)
  target_compile_options(pagan_route_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_route_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME route COMMAND pagan_route_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/route_shots)
  # Walk to act (app/auto_walk.cpp): use / pick up after walking there; water or input stop it.
  add_executable(pagan_autowalk_tests ${CMAKE_SOURCE_DIR}/tests/test_autowalk.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_autowalk_tests PRIVATE pagan_core)
  target_compile_options(pagan_autowalk_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_autowalk_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME autowalk COMMAND pagan_autowalk_tests ${PAGAN_GAME_DIR} ${CMAKE_BINARY_DIR}/autowalk_shots)
  # The options page in the menu book (MenuGump kOptions): layout inside the pages, every row.
  add_executable(pagan_options_tests ${CMAKE_SOURCE_DIR}/tests/test_options.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_options_tests PRIVATE pagan_core)
  target_compile_options(pagan_options_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_options_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME options COMMAND pagan_options_tests ${PAGAN_GAME_DIR} ${CMAKE_BINARY_DIR}/options_shots
    ${CMAKE_BINARY_DIR}/options_saves)
  # Companions (game/party.*, companion_ai.cpp, companion_talk.cpp, party_gumps.cpp, data/companions.json).
  add_executable(pagan_party_tests ${CMAKE_SOURCE_DIR}/tests/test_party.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_party_tests PRIVATE pagan_core)
  target_compile_options(pagan_party_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_party_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME party COMMAND pagan_party_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/party_shots
    ${CMAKE_BINARY_DIR}/party_saves)
  # Companions' homes on their maps, debug mode's beach, old saves (tests/test_homes.cpp).
  add_executable(pagan_homes_tests ${CMAKE_SOURCE_DIR}/tests/test_homes.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_homes_tests PRIVATE pagan_core)
  target_compile_options(pagan_homes_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_homes_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME homes COMMAND pagan_homes_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/homes_shots
    ${CMAKE_BINARY_DIR}/homes_saves)
  # Levels and XP (game/leveling.*, tests/test_leveling.cpp).
  add_executable(pagan_leveling_tests ${CMAKE_SOURCE_DIR}/tests/test_leveling.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_leveling_tests PRIVATE pagan_core)
  target_compile_options(pagan_leveling_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_leveling_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME leveling COMMAND pagan_leveling_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/leveling_shots
    ${CMAKE_BINARY_DIR}/leveling_saves)
  # The new creatures and one level per monster type (game/creatures.*, tests/test_creatures.cpp).
  add_executable(pagan_creatures_tests ${CMAKE_SOURCE_DIR}/tests/test_creatures.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_creatures_tests PRIVATE pagan_core)
  target_compile_options(pagan_creatures_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_creatures_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME creatures COMMAND pagan_creatures_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/creatures_shots
    ${CMAKE_BINARY_DIR}/creatures_saves)
  # Virtues: the score store, the octagon and the level line on the character sheet (game/virtues.*, tests/test_virtues.cpp).
  add_executable(pagan_virtues_tests ${CMAKE_SOURCE_DIR}/tests/test_virtues.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_virtues_tests PRIVATE pagan_core)
  target_compile_options(pagan_virtues_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_virtues_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME virtues COMMAND pagan_virtues_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/virtues_shots
    ${CMAKE_BINARY_DIR}/virtues_saves)
  # World stages: Pagan transformed as the virtues grow (game/world_stages.*, tests/test_stages.cpp).
  add_executable(pagan_stages_tests ${CMAKE_SOURCE_DIR}/tests/test_stages.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_stages_tests PRIVATE pagan_core)
  target_compile_options(pagan_stages_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_stages_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME stages COMMAND pagan_stages_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/stages_shots
    ${CMAKE_BINARY_DIR}/stages_saves)
  # Known as the Avatar: Devon's celebration, the usecode's words for the Avatar, the child (tests/test_avatar.cpp).
  add_executable(pagan_avatar_tests ${CMAKE_SOURCE_DIR}/tests/test_avatar.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_avatar_tests PRIVATE pagan_core)
  target_compile_options(pagan_avatar_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_avatar_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME avatar COMMAND pagan_avatar_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/avatar_saves)
  # Leveling stage 4: trainers, tomes, elixirs, ordeals, quest stats (game/stat_growth.*, tests/test_growth.cpp).
  add_executable(pagan_growth_tests ${CMAKE_SOURCE_DIR}/tests/test_growth.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_growth_tests PRIVATE pagan_core)
  target_compile_options(pagan_growth_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_growth_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME growth COMMAND pagan_growth_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/growth_shots
    ${CMAKE_BINARY_DIR}/growth_saves)
  # The spellbook (game/spellbook.*, tests/test_spellbook.cpp).
  add_executable(pagan_spellbook_tests ${CMAKE_SOURCE_DIR}/tests/test_spellbook.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_spellbook_tests PRIVATE pagan_core)
  target_compile_options(pagan_spellbook_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_spellbook_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME spellbook COMMAND pagan_spellbook_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/spellbook_shots
    ${CMAKE_BINARY_DIR}/spellbook_saves)
  # Leveling stage 5: companion spells and reagents (game/companion_magic.*, tests/test_magic.cpp).
  add_executable(pagan_magic_tests ${CMAKE_SOURCE_DIR}/tests/test_magic.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_magic_tests PRIVATE pagan_core)
  target_compile_options(pagan_magic_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_magic_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME magic COMMAND pagan_magic_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/magic_shots
    ${CMAKE_BINARY_DIR}/magic_saves)
  # Travel banter between companions and the INT-gated NPC topics (game/banter.*, tests/test_banter.cpp).
  add_executable(pagan_banter_tests ${CMAKE_SOURCE_DIR}/tests/test_banter.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_banter_tests PRIVATE pagan_core)
  target_compile_options(pagan_banter_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_banter_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME banter COMMAND pagan_banter_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/banter_saves)
  # Leveling stage 3: the headless fight simulator and the balance table (tests/test_balance.cpp).
  add_executable(pagan_balance_tests ${CMAKE_SOURCE_DIR}/tests/test_balance.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_balance_tests PRIVATE pagan_core)
  target_compile_options(pagan_balance_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_balance_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME balance COMMAND pagan_balance_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data 24 check)
  # Turn-based combat (game/turn_combat.*, app/turn_ui.*): fights, Party Auto, input, end conditions.
  # The Aerial Servant: the original fetch and the fighting servant (game/servant.*, tests/test_servant.cpp).
  add_executable(pagan_servant_tests ${CMAKE_SOURCE_DIR}/tests/test_servant.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_servant_tests PRIVATE pagan_core)
  target_compile_options(pagan_servant_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_servant_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME servant COMMAND pagan_servant_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/servant_shots
    ${CMAKE_BINARY_DIR}/servant_saves)
  # Item powers: the Skull of Quakes (game/item_powers.*, tests/test_skull.cpp).
  add_executable(pagan_skull_tests ${CMAKE_SOURCE_DIR}/tests/test_skull.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_skull_tests PRIVATE pagan_core)
  target_compile_options(pagan_skull_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_skull_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME skull COMMAND pagan_skull_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/skull_shots
    ${CMAKE_BINARY_DIR}/skull_saves)

  add_executable(pagan_turn_tests ${CMAKE_SOURCE_DIR}/tests/test_turn.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_turn_tests PRIVATE pagan_core)
  target_compile_options(pagan_turn_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_turn_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME turn COMMAND pagan_turn_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/turn_shots
    ${CMAKE_BINARY_DIR}/turn_saves)
  # The HUD side panel (app/hud_panel.*, gfx/screen_layout.*, game/game_log.*; PLAN-HUD.md) and the
  # turn-based Move's destination cursor.
  add_executable(pagan_hud_tests ${CMAKE_SOURCE_DIR}/tests/test_hud.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_hud_tests PRIVATE pagan_core)
  target_compile_options(pagan_hud_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_hud_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME hud COMMAND pagan_hud_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/hud_shots
    ${CMAKE_BINARY_DIR}/hud_saves)
  # Engine-side dialogue topics added to original NPCs (game/npc_dialog.*, data/dialog/*.json):
  # Gwillim's story-stage topics, the hook into his own answer menu, mouse / keys / pad.
  add_executable(pagan_dialog_tests ${CMAKE_SOURCE_DIR}/tests/test_dialog.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_dialog_tests PRIVATE pagan_core)
  target_compile_options(pagan_dialog_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_dialog_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME dialog COMMAND pagan_dialog_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/dialog_shots
    ${CMAKE_BINARY_DIR}/dialog_saves)
  # Easy ledges (game/ledge.*): real ledges found from the maps' geometry, held jump from many
  # places in front of them; unreachable ones never climbed.
  add_executable(pagan_ledge_tests ${CMAKE_SOURCE_DIR}/tests/test_ledges.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_ledge_tests PRIVATE pagan_core)
  target_compile_options(pagan_ledge_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_ledge_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME ledges COMMAND pagan_ledge_tests ${PAGAN_GAME_DIR} ${CMAKE_BINARY_DIR}/ledge_shots)
  # Original bug fixes (world, physics, audio; core/bugfix.h, the "Original bug fixes" option):
  # each bug reproduced with the option Off and fixed with it On; the map data fixes.
  add_executable(pagan_bugfix_tests ${CMAKE_SOURCE_DIR}/tests/test_bugfixes.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_bugfix_tests PRIVATE pagan_core)
  target_compile_options(pagan_bugfix_tests PRIVATE ${PG_WARNINGS})
  target_compile_definitions(pagan_bugfix_tests PRIVATE PAGAN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  set_target_properties(pagan_bugfix_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME bugfixes COMMAND pagan_bugfix_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/bugfix_shots)
  # The buried cave mouth on map 57 (world/map_patches.*, data/patches/map_content.json): the data, the
  # option, no overlaps, and walking, jumping and pathfinding at it from every side.
  add_executable(pagan_underworld_tests ${CMAKE_SOURCE_DIR}/tests/test_underworld.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_underworld_tests PRIVATE pagan_core)
  target_compile_options(pagan_underworld_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_underworld_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME underworld COMMAND pagan_underworld_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/underworld_shots
    ${CMAKE_BINARY_DIR}/underworld_saves)
  # Original bug fixes in the story scripts and text (usecode/uc_patches.*, data/patches/*.json):
  # each bug reproduced with the option Off and fixed with it On.
  add_executable(pagan_storyfix_tests ${CMAKE_SOURCE_DIR}/tests/test_storyfixes.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_storyfix_tests PRIVATE pagan_core)
  target_compile_options(pagan_storyfix_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_storyfix_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME storyfixes COMMAND pagan_storyfix_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data
    ${CMAKE_BINARY_DIR}/storyfix_saves)
  # v2.12 features and the restored undead invasion (game/variants.*, NOTES.md).
  add_executable(pagan_v212_tests ${CMAKE_SOURCE_DIR}/tests/test_v212.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_v212_tests PRIVATE pagan_core)
  target_compile_options(pagan_v212_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_v212_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME v212 COMMAND pagan_v212_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/v212_shots
    ${CMAKE_BINARY_DIR}/v212_saves)
  # The stitched world (world/world_layout.*, world/world_space.*, game/world_stitch.cpp):
  # layout, seam crossings, links, companions / combat at a seam, saves, streaming, the
  # critical path in both modes.
  add_executable(pagan_world_tests ${CMAKE_SOURCE_DIR}/tests/test_world_space.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_world_tests PRIVATE pagan_editor pagan_core)
  target_compile_options(pagan_world_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_world_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME world COMMAND pagan_world_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/world_shots
    ${CMAKE_BINARY_DIR}/world_saves)
  # The whole story from a new game to the ending movie (tests/test_story.cpp), chapter by chapter.
  add_executable(pagan_story_tests ${CMAKE_SOURCE_DIR}/tests/test_story.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_story_tests PRIVATE pagan_core)
  target_compile_options(pagan_story_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_story_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME story COMMAND pagan_story_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/story_saves)
  # The Ceremony of Eternity played as a player meets it, in both world modes (specs/world/scenes.md 1).
  add_test(NAME ritual COMMAND pagan_story_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/ritual_saves ritual)
  add_test(NAME ritual_stitched COMMAND pagan_story_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/ritual_saves ritual stitched)
  # The debug menu's story jump, levels and virtues pages (data/story_jump.json and the checkpoints
  # pack data/story/checkpoints.pgs that ctest story writes; tests/test_storyjump.cpp).
  add_executable(pagan_storyjump_tests ${CMAKE_SOURCE_DIR}/tests/test_storyjump.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_storyjump_tests PRIVATE pagan_core)
  target_compile_options(pagan_storyjump_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_storyjump_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME storyjump COMMAND pagan_storyjump_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data
    ${CMAKE_BINARY_DIR}/storyjump_shots ${CMAKE_BINARY_DIR}/storyjump_saves)
  set_tests_properties(storyjump PROPERTIES DEPENDS story)
  # Map dresser (src/editor): the tile-kit miner, the dresser CLI and its tests.
  add_executable(pagan_tilekit ${CMAKE_SOURCE_DIR}/src/editor/tilekit_main.cpp ${CMAKE_SOURCE_DIR}/src/platform/screenshot.cpp)
  target_link_libraries(pagan_tilekit PRIVATE pagan_editor)
  add_executable(pagan_dress ${CMAKE_SOURCE_DIR}/src/editor/dress_main.cpp ${CMAKE_SOURCE_DIR}/src/platform/screenshot.cpp)
  target_link_libraries(pagan_dress PRIVATE pagan_editor)
  add_executable(pagan_dress_tests ${CMAKE_SOURCE_DIR}/tests/test_dress.cpp ${CMAKE_SOURCE_DIR}/src/platform/screenshot.cpp)
  target_link_libraries(pagan_dress_tests PRIVATE pagan_editor)
  # Custom actor art (data/shapes/, NOTES.md "Custom shapes"): contact sheets and the painter.
  add_executable(pagan_skin ${CMAKE_SOURCE_DIR}/src/editor/skin_main.cpp ${CMAKE_SOURCE_DIR}/src/platform/screenshot.cpp)
  target_link_libraries(pagan_skin PRIVATE pagan_editor)
  # The stitched world's layout tools: the validator (ctest layout_check), footprints, generate.
  add_executable(pagan_layout ${CMAKE_SOURCE_DIR}/src/editor/layout_main.cpp)
  target_link_libraries(pagan_layout PRIVATE pagan_editor)
  add_test(NAME layout_check COMMAND pagan_layout ${PAGAN_GAME_DIR} check ${CMAKE_SOURCE_DIR}/data/world_layout.json)
  foreach(t pagan_tilekit pagan_dress pagan_dress_tests pagan_skin pagan_layout)
    target_include_directories(${t} SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/sdl2)
    target_compile_options(${t} PRIVATE ${PG_WARNINGS})
    set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  endforeach()
  add_test(NAME dress COMMAND pagan_dress_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data/tilekit.json
    ${CMAKE_SOURCE_DIR}/tests/data/areas ${CMAKE_BINARY_DIR}/dress_shots)
  # Edit mode (src/editor/edit_mode.*, area_model.*, editor_storage.*): paint model, storage, UI shots.
  add_executable(pagan_editor_tests ${CMAKE_SOURCE_DIR}/tests/test_editor.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_editor_tests PRIVATE pagan_editor pagan_core)
  target_compile_options(pagan_editor_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_editor_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME editor COMMAND pagan_editor_tests ${PAGAN_GAME_DIR} ${CMAKE_BINARY_DIR}/editor_shots
    ${CMAKE_BINARY_DIR}/editor_tmp ${CMAKE_SOURCE_DIR}/data/tilekit.json)
  add_test(NAME editor_shot COMMAND ${CMAKE_COMMAND} -E env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy HOME=${CMAKE_BINARY_DIR}
    $<TARGET_FILE:pagan> --game ${PAGAN_GAME_DIR} --edit --atlas-layout ${CMAKE_BINARY_DIR}/editor_data/atlas_layout.json
    --screenshot ${CMAKE_BINARY_DIR}/test_edit.png --frames 3)
  # Any copy of the game (data/compat.*, tests/test_compat.cpp): a changed usecode byte, a changed map
  # record, missing optional files, lower-case names, each in a scratch copy of the game folder.
  add_executable(pagan_compat_tests ${CMAKE_SOURCE_DIR}/tests/test_compat.cpp ${PG_GAMEMODE_SOURCES})
  target_link_libraries(pagan_compat_tests PRIVATE pagan_core)
  target_compile_options(pagan_compat_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_compat_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME compat COMMAND pagan_compat_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/compat_tmp)
  # World atlas and zoom (world/atlas_layout.*, world/tile_cache.*).
  add_executable(pagan_atlas_tests ${CMAKE_SOURCE_DIR}/tests/test_atlas.cpp)
  target_link_libraries(pagan_atlas_tests PRIVATE pagan_core)
  target_compile_options(pagan_atlas_tests PRIVATE ${PG_WARNINGS})
  set_target_properties(pagan_atlas_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  add_test(NAME atlas COMMAND pagan_atlas_tests ${PAGAN_GAME_DIR} ${CMAKE_SOURCE_DIR}/data/atlas_layout.json)
  # Zooming out and back in must leave the 1x view pixel-identical.
  add_test(NAME zoom_plain COMMAND ${CMAKE_COMMAND} -E env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
    $<TARGET_FILE:pagan> --game ${PAGAN_GAME_DIR} --view-map 40 --mouse 200,60 --screenshot ${CMAKE_BINARY_DIR}/test_zoom_a.png --frames 3)
  add_test(NAME zoom_roundtrip COMMAND ${CMAKE_COMMAND} -E env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
    $<TARGET_FILE:pagan> --game ${PAGAN_GAME_DIR} --view-map 40 --mouse 200,60 --zoom-roundtrip --screenshot ${CMAKE_BINARY_DIR}/test_zoom_b.png --frames 3)
  add_test(NAME zoom_same COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_BINARY_DIR}/test_zoom_a.png ${CMAKE_BINARY_DIR}/test_zoom_b.png)
  set_tests_properties(zoom_same PROPERTIES DEPENDS "zoom_plain;zoom_roundtrip")
  add_test(NAME atlas_shot COMMAND ${CMAKE_COMMAND} -E env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
    $<TARGET_FILE:pagan> --game ${PAGAN_GAME_DIR} --atlas --screenshot ${CMAKE_BINARY_DIR}/test_atlas.png --frames 2)
  add_test(NAME screenshot COMMAND ${CMAKE_COMMAND} -E env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
    $<TARGET_FILE:pagan> --game ${PAGAN_GAME_DIR} --screenshot ${CMAKE_BINARY_DIR}/test_shot.png --frames 3)
  add_test(NAME game_run COMMAND ${CMAKE_COMMAND} -E env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
    $<TARGET_FILE:pagan> --game ${PAGAN_GAME_DIR} --auto-ui --screenshot ${CMAKE_BINARY_DIR}/test_game.png --frames 900)
  add_test(NAME viewer_shot COMMAND ${CMAKE_COMMAND} -E env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
    $<TARGET_FILE:pagan> --game ${PAGAN_GAME_DIR} --view-map 40 --screenshot ${CMAKE_BINARY_DIR}/test_viewer.png --frames 3)
  include(${CMAKE_SOURCE_DIR}/private-tests.cmake OPTIONAL)
endif()
