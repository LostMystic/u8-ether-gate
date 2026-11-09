#!/bin/sh
# Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
# Serves the web build on http://localhost:8619/ (or the port given).
cd "$(dirname "$0")/../build-web/web" || { echo "build it first: web/build.sh"; exit 1; }
PORT="${1:-8619}"
echo "http://localhost:$PORT/  (e.g. /?map=40)"
exec python3 -m http.server "$PORT" --bind 127.0.0.1
