# Third-party parts

U8 Ether Gate itself is under the GNU GPL v3 or later (`LICENSE`). These parts keep their own licences,
all of which allow this use. The licence text is in the file named.

| Part | Where | Licence | Text |
| --- | --- | --- | --- |
| SDL2 headers (and, in the Linux package, the SDL2 library; on the web, Emscripten's SDL2 port) | `third_party/sdl2`, `lib/` | zlib | `third_party/sdl2/LICENSE.txt` (Linux package: `lib/SDL2-LICENSE.txt`) |
| stb_image, stb_image_write (Sean Barrett) | `third_party/stb` | MIT or public domain | end of each header |
| nlohmann/json (Niels Lohmann) | `third_party/json` | MIT | head of `json.hpp` |
| TinySoundFont, TinyMidiLoader (Bernhard Schelling) | `third_party/tsf` | MIT | head of each header |
| The C++ runtime (libstdc++, libgcc_s), Linux package only | `lib/` | GPL v3 with the GCC Runtime Library Exception | https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html |
| A subset of the GeneralUser GS v2.0.3 SoundFont (S. Christian Collins, http://www.schristiancollins.com) | `data/soundfont/u8_gm.sf2` | GeneralUser GS License v2.0 | `data/soundfont/GeneralUser-GS-LICENSE.txt`; how the subset was made: `data/soundfont/README.md` |

The SoundFont is a data file used by the program, under its author's own licence and not under the GPL.
Any General MIDI SoundFont can replace it (`--soundfont FILE`, native). The author notes that some of its
samples came from other free banks and that he cannot be fully sure of every origin.

Ultima VIII's own files are not part of this project in any form. You bring your own copy.
