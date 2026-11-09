#!/bin/sh
# Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
# Runs the native engine. It finds the game folder by itself (--game DIR to say). Extra arguments go to pagan,
# e.g. ./run.sh --view-map 40, or ./run.sh --view-map 3 --at 14527,5887,8
cd "$(dirname "$0")"
[ -x build/bin/pagan ] || ./build.sh
exec build/bin/pagan "$@"
