# Painted areas and dressed areas (format v1)

Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.

This is the contract between the **edit mode** (which paints areas and saves them) and the
**map dresser** (`src/editor/dresser.*`, CLI `pagan_dress`), which turns a painted area into
real map items. Version 1 is stable: later versions only add optional fields, and readers
ignore fields they do not know.

## 1. Painted area (`*.area.json`, input to the dresser)

```json
{
  "format": "pagan-area",
  "version": 1,
  "name": "Lithos cave",
  "theme": "cave",
  "seed": 1234,
  "cell": 128,
  "base_z": 0,
  "level_z": 8,
  "origin": [16384, 16384, 0],
  "rows": [
    "##########",
    "#........#",
    "#...~~...#",
    "#......../....",
    "####+#####"
  ],
  "heights": [
    "0000000000",
    "0000000000",
    "0000000000",
    "00000000000000",
    "0000000000"
  ],
  "themes": ["...optional rows of legend letters..."],
  "theme_legend": {"c": "cave", "t": "tenebrae"},
  "pois": [
    {"id": "p1", "x": 4, "y": 1, "note": "shrine to Lithos, ruined", "tags": ["shrine", "earth"]}
  ]
}
```

| Field | Required | Meaning |
| --- | --- | --- |
| `format` | yes | Always `"pagan-area"`. |
| `version` | yes | `1`. |
| `name` | no | Free text. |
| `theme` | yes | Default theme of every cell (see section 3 for the names). |
| `seed` | no | Default seed (the CLI's `--seed` and the library call override it). Default 1. |
| `cell` | no | Cell size in world units, x and y. Default **128**: the footprint of the dominant floor tile (4 x 4 footprint units = 128 x 128; about 250,000 of the original maps' floor tiles). Only 128 is supported by the v1 dresser; other values are rejected. |
| `base_z` | no | World z of height level 0. Default 0. |
| `level_z` | no | World z per height level. Default 8 (the step an actor walks up without climbing; physics.md 6). Floors in the original maps sit at multiples of 8 (0, 8, 48, 56 are the most common). |
| `origin` | no | Where the area's (0, 0, 0) goes in the world (or in a map) when it is placed. The dresser ignores it (its output is area-local); the editor and the stitcher use it. |
| `rows` | yes | The cell grid, one string per row. Row index = **cell y** (world +y), character index = **cell x** (world +x). Rows may differ in length; missing cells are void. |
| `heights` | no | Same shape as `rows`; each character is a base-36 level (`0`-`9`, `a`-`z` = 10-35). Missing = 0. |
| `themes` | no | Same shape as `rows`; each character is a key of `theme_legend`. `.` or a space or a missing character = the area's `theme`. |
| `theme_legend` | with `themes` | Map from a one-letter key to a theme name. |
| `pois` | no | Points of interest (section 2). |
| `styles` | no | Overrides of the theme's choices, by kit id: `{"wall": "ws149", "ledge": "ws26", "floor": "fs242", "interior_floor": "fs163", "water": "fs347", "roof": "fs412"}` (ids and labels in `data/tilekit.json` `walls` / `floors`). Unknown ids are warned about and ignored. |

Cell `(cx, cy)` covers world x in `[cx*cell, (cx+1)*cell)` and y in `[cy*cell, (cy+1)*cell)`,
area-local. On screen +x runs down-right and +y down-left (screen_x = (x - y)/4,
screen_y = (x + y)/8 - z), so row 0 is the top-right edge of the area and column 0 the
top-left edge. "North" in the original's directions is -y.

### Cell codes

| Char | Kind | Dressing |
| --- | --- | --- |
| ` ` (space) | void | Nothing (outside the area, or a chasm). Floors next to it get edge trim where the theme has it. |
| `.` | floor | Floor tiles of the cell's theme at the cell's height. |
| `#` | wall | A wall segment along the cell; straights, corners, T-junctions and ends are chosen from the neighbouring wall cells. A floor is laid under it. |
| `+` | entrance | A door in the wall line (a working door: the original door classes). Must have wall cells on two opposite sides (it sits in a wall line). |
| `/` | passage | An open gap in a wall line (archway or just open floor). |
| `~` | water | Water tiles of the theme (not walkable). Floors next to it get water-edge trim where the theme has it. |
| `=` | stairs | v1: a floor at its own height that gets no riser; paint its height between its neighbours (steps of at most 8 are walkable). The kit has no stair pieces. |
| `x` | void (chasm) | Same as space, but marks a deliberate chasm (kept for the editor). |

Any other character is read as floor with a warning.

## 2. Points of interest

```json
{"id": "p1", "x": 4, "y": 1, "note": "shrine to Lithos, ruined", "tags": ["shrine"], "prefab": "", "radius": 2}
```

| Field | Required | Meaning |
| --- | --- | --- |
| `id` | no | Any string, copied to the output items. Default `poi<N>`. |
| `x`, `y` | yes | Cell of the marker (must be a floor-like cell). |
| `note` | no | Free text. Its words are matched against the prefab labels and keywords in the tile kit. |
| `tags` | no | Words that count double in the match (e.g. `"grave"`, `"camp"`, `"light"`). |
| `prefab` | no | Force a prefab by its id in `data/tilekit.json` (Claude or a person refining the choice). |
| `radius` | no | Cells around the marker the prefab may use (default 2). |

The dresser picks the best-scoring prefab (keyword and tag matches, theme match as a tie-break,
seeded choice among equals), places it centred on the marker where it does not overlap walls,
doors or water, and reports the choice.

## 3. Themes

Theme names come from `data/tilekit.json` (`themes`). The v1 kit has (at least):
`cave`, `catacombs`, `tenebrae` (town), `cemetery`, `plateau`, `lava`, `beach`, `valley`
(grassland and roads), `fortress`, `lake`, `argentrock`, `shrine`, `ether` (the planes). An unknown theme falls back to the closest theme by the kit's
`fallback` list, else `cave`.

## 4. Dressed area (`*.dressed.json`, output of the dresser)

```json
{
  "format": "pagan-dressed",
  "version": 1,
  "name": "Lithos cave",
  "seed": 1234,
  "cell": 128,
  "origin": [16384, 16384, 0],
  "size": [1792, 1280],
  "items": [
    {"shape": 34, "frame": 3, "x": 127, "y": 127, "z": 0, "flags": 0, "quality": 0,
     "npcnum": 0, "mapnum": 0, "role": "floor", "cell": [0, 0], "poi": ""}
  ],
  "pois": [{"id": "p1", "prefab": "shrine_3", "label": "shrine", "score": 6, "placed": true}],
  "entrances": [[4, 4]],
  "warnings": [],
  "validation": {"ok": true, "floor_covered": true, "walls_closed": true, "overlaps": 0,
                 "unsupported": 0, "walkable": true, "unreachable_pois": []}
}
```

- `items[]` are original-style item records in **area-local world units**: (x, y, z) is the
  item's stored corner (largest x and y, smallest z; maps.md 1), `flags` are item flags
  (FLIPPED 0x0020 etc.), `quality`/`npcnum`/`mapnum` as in maps.md 2 (doors keep their
  quality, eggs their ranges). Add `origin` to place them in a map.
- `role`, `cell` and `poi` are informational (what the dresser meant; which POI an item
  belongs to). Loaders ignore them.
- Items are listed floors first, then trims, walls, doors, POI items, in a stable order; the
  same input and seed give byte-identical output.
- `validation` is the result of the dresser's own checks (placement rules of
  specs/world/physics.md 2, floor coverage, closed walls, walkability from the entrances to
  every POI).

## 5. Library and CLI

```cpp
#include "editor/dresser.h"
pg::TileKit kit;  pg::load_tilekit("data/tilekit.json", kit, err);
pg::PaintedArea area; pg::parse_area(json_text, area, err);
pg::DressResult r = pg::dress(area, kit, seed);   // no SDL, no game data needed
std::string out = pg::dressed_to_json(area, r);
pg::Validation v = pg::validate(r, area, kit);   // also run by dress(); uses the kit's shape table
```

- `pagan_dress area.json out.json [--seed N] [--png preview.png] [--kit data/tilekit.json] [--game DIR]`
- `pagan_tilekit [--game DIR] [--out data/tilekit.json] [--report report.txt]` rebuilds the kit.
- The viewer shows a dressed area on its own (empty map 254, the area at its origin, or at
  4096, 4096 when the origin is 0): `pagan --view-area out.json`; with `--view-map N` it is laid
  over map N instead.
- `pagan_dress` exits 0 when validation passes, 3 when it found problems, 1/2 on errors.

## 6. What a painted cell becomes (v1)

- **Floors**: the theme's `floor` (enclosed rooms: `interior_floor`), one 128 tile per cell at
  the cell's z, chosen by weight and by how often the original puts each tile next to the west
  and north neighbours; identical neighbours are avoided.
- **Walls**: thin walls along the middle of wall cells (32-unit slot 1 of the cell): posts at
  corners, T-junctions, crosses and ends, straight pieces of 128/96/64/32 between them.
- **Entrances** (`+`): a closed, unlocked door of the original door classes (DOOR_EW 69 in a
  wall along x, DOOR_NS 68 along y) with a jamb each side. **Passages** (`/`): a 160-wide gap
  with end posts.
- **Water** (`~`): water tiles; next to land, the theme's shore tiles (8 high) for that side.
- **Height changes** of more than 8 between neighbouring cells: risers on the higher cell's
  edge, stacked from wall pieces whose heights add up to the drop. Edges over void stay open.
- **Roofs**: rooms enclosed by walls/doors get one roof tile kind over the room and its walls at
  floor z + wall height (themes that have roofs). Roofs hide in the game when you go inside.
- **POIs**: a prefab from the kit placed in floor cells near the marker; else a light/decoration
  marker. **Scatter**: decorations, plants and lights on open floor (never in corridors or
  next to entrances).
