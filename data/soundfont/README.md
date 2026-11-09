# Music SoundFont

`u8_gm.sf2` (about 10 MB) is a subset of **GeneralUser GS v2.0.3** by S. Christian Collins
(http://www.schristiancollins.com), made with `engine/tools/sf2_subset.py`: only the 35 General MIDI
programs and the two GS drum kits (Standard 0, Orchestra 48) that the U8 songs and transitions in
MUSIC.FLX select, drum-kit zones limited to the 16 drum notes the songs play, and samples above
30 kHz halved in rate. Regenerate with:

    P=0,1,11,13,14,19,40,41,44,45,46,47,48,49,52,55,56,57,58,60,61,68,69,72,73,74,76,79,82,88,89,115,116,119,120
    D=27,35,36,38,41,52,54,55,57,58,59,61,63,75,81,83
    python3 -I tools/sf2_subset.py GeneralUser-GS.sf2 data/soundfont/u8_gm.sf2 \
        --programs $P --kits 0,48 --drum-notes $D --halve-rate-above 30000

**Licence** (`GeneralUser-GS-LICENSE.txt`, "License v2.0"): free to use "without restriction"
for music creation, private or commercial, and the author invites use "in your software projects"
and modification of "the SoundFont bank or its packaging". It is not an OSI licence and has no
copyleft. Caveat stated by the author: some samples came from other free banks and their origin
cannot be guaranteed 100%; he reports no complaints since 2000. Review this before a commercial
release; any GM/GS SoundFont can replace it (`--soundfont FILE`, native).
