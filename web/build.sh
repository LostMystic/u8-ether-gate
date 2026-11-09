#!/bin/bash
# Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
# Builds the browser version into build-web/web/ (pagan.html = index.html, .js, .wasm, exit.html,
# packs.js, version.json and files/: the data packs, see tools/web_pack.py). Set EMSDK to your
# Emscripten SDK folder (a checkout may set it in web/private.env); this never downloads one.
set -e
cd "$(dirname "$0")/.."
[ -f web/private.env ] && . web/private.env
: "${EMSDK:?Set EMSDK to the folder of your Emscripten SDK}"
source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release "$@" >/dev/null
cmake --build build-web -j"$(nproc)"
cp -f build-web/web/pagan.html build-web/web/index.html
ls -la build-web/web
