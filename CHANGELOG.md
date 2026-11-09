# U8 Ether Gate - what's new

## Unreleased

## 0.25.1 - 2026-11-09
- The world now uses the original maps by default: each part of Tenebrae loads on its own, as in the original game.
  The joined-up world showed parts of neighbouring districts drawn over Central Tenebrae's river and banks. It is
  still there to try (Options, Game, World: Stitched) while that is being fixed; if you switched it on yourself,
  your choice is kept.

## 0.25.0 - 2026-11-09
- New games always start from the very beginning, from your copy's own new-game save (SAVEGAME/U8SAVE.000), as the
  original does. A copy you have played in DOS (whose GAMEDAT folder holds that old game) now starts fresh, keeps all
  the fixes, and a copy without a GAMEDAT folder works too.
- Choosing the game folder: you can pick the folder around it (a store's or a DOSBox package's), and the game is
  found up to three folders down.

## 0.24.0 - 2026-11-08
- The red X now says why: "Too heavy for you: load 452, limit 440", "Not enough room in there", "Too far away: walk
  closer to it first", "Too far to throw it there" and more, beside the pointer while you drag and for a moment after.
- Looting: double-click an item in a body, chest or bag that isn't yours and it goes straight into your backpack (a bag
  or box inside still opens). A TAKE ALL button next to SORT takes everything that fits and says what it had to leave.
- Spellbook: a Scrolls section. Every scroll you carry is listed, one cast per scroll; casting from the book uses
  the scroll up (except the Scroll of Dispelling Magic Portals, which the story needs again). When you have none
  left it leaves the book, and comes back when you pick one up.
- Turn-based combat: the Avatar's Cast list is now the spellbook, with the casts left and why a spell can't be cast
  (every spell, unlimited, with the debug option on).
- Rock Flesh works visibly: the Avatar casts, you pick him or an ally (not a foe), and the target turns to grey stone
  and takes no damage for 2 minutes, or 12 rounds of turn-based combat (with "Original bug fixes" on).

## 0.23.0 - 2026-11-08
- A spellbook (G): your spells in sections by school (Necromancy, Theurgy, Sorcery, Thaumaturgy, and a Tempest
  section for the water spells to come), four to a page. A spell is written in the first time you make it (a
  necromancy token from a bag of reagents, a focus charged at a pentagram) or cast it yourself; scrolls don't count.
- Each spell in the book shows how many casts you have from your tokens, charged foci and books and the reagents you
  carry, and whether your mana is enough. Double-click a spell to cast it: tokens and charges are used first, then
  reagents; with no reagents or too little mana nothing is used and the book says why.
- Debug menu: "Infinite spells" lists every spell in the book and casts any of them for free; turn it off and the book
  is back as it was (debug casts never write a spell in).
- Debug menu: "Send all companions home" sends every companion back to their home at once.
- The lever by the catacomb gate no longer turns the screen upside down (clicks missed what was shown, worse when
  zoomed); it sets off a burst of sparkles instead (with "Original bug fixes" on).

## 0.22.1 - 2026-11-08
- Lothian's ceremony in the Cemetery tomb now plays inside, at the altar, when you bring the dagger back over the
  East Road into the stitched world: Vividos waits by the altar instead of outside the door.

## 0.22.0 - 2026-11-08
- Using or picking up something a little out of reach: the Avatar now walks over to it and does it (the red X only when it is too far or the way is unsafe).
- Conversations: the last bit of a long line ("...forgive me.") no longer flashes past before the questions come back.
- Virtues: each virtue you earn now floats up over the Avatar's head ("Humility +4") once the conversation
  or scene is over, then fades away.
- Debug menu: Give item now gives what you click at once, coins 500 at a time, and says what you got.

## 0.21.0 - 2026-11-08
- Debug menu: "Jump to story chapter..." puts you at the start of any chapter, forward or back, with the world as it
  was then and the level, weapons, armour, obsidian and potions a player would have.
- Debug menu: "Levels..." sets the Avatar's and each companion's level up or down.
- Debug menu: the virtues page also sets infamy, marks, and typical scores for the chapter.

## 0.20.0 - 2026-11-08
- Aerial Servant: cast on a hostile creature, the servant now fights it: it flies over and strikes a few times for
  small damage, the creature turns on it, and a hit or two blows it away (with "Item powers" on).
- The Key of the Scion now makes necromancy spells from a bag too, like the other two keys; it still opens doors and
  chests (with "Original bug fixes" on).
- Fixed: the sorcerers' first test could never be passed if you gave up or left the house after one or two of the
  three spells; starting it again now starts it afresh.
- Virtues: Pagan slowly changes as your virtues grow (at 20, 40 and 60 points). The outdoor maps turn warmer and
  brighter, and now and then soft rays of sunlight fall across them, more often as you rise. The Underworld and the
  Planes stay as grim as ever.
- Virtues: fewer monsters lurk on the roads around Tenebrae and in Herdsman's Valley's lower track at higher stages
  (never none), and once you have laid enough of the dead to rest, fewer rise in the Cemetery. The catacombs stay
  dangerous.
- Virtues: at the later stages Tenebrae and Herdsman's Valley play a lighter tune from the game's own music.
- Virtues: once the rain storm comes, it is milder near Tenebrae as your virtues grow: less grey, fewer flashes and
  thunderclaps, and at the later stages the mornings there are clear.
- Virtues: at night Tenebrae's street lamps, wall lamps, braziers and hearths cast a warm glow.
- Virtues: birdsong and the sound of the sea near the towns, crickets at night, and once enough of the dead are at
  peace the Cemetery grows quieter and the crickets come back.
- Virtues: as trade recovers, Darion, Rhian and Mythran give a little of your obsidian back on every sale (5% to 15%).
- Virtues and Leveling: the XP of the monsters that no longer roam near Tenebrae is not lost. It waits and is paid out
  as a bonus when you fight on the frontier: Daemon's Crag, the Pit of the Dead, the Planes and the wilds.
- Virtues: torax herds graze in Herdsman's Valley and small creatures scurry about at the later stages, and children
  play in Tenebrae's streets by day.
- Virtues: townsfolk now and then greet you for what you have done ("You're the one who spoke up for Toran's family."),
  and the guards turn polite.
- Virtues: once Devon rules Tenebrae and your virtues are high, he sends for you to celebrate and asks what you want to
  be known as. Answer "The Avatar" and from then on the people of Pagan call you Avatar instead of stranger.
- Virtues: the child whose mommy told her never to talk to strangers will talk to the Avatar, and has a small favour
  to ask (Humility, if you let her lead).
- Virtues: banishing a daemon with Banish Daemon now gives the same XP as slaying it, and raises Spirituality.
- Virtues: every virtue can now be earned. Justice: ask Devon to clear Toran's name. Sacrifice: give Rhian obsidian
  for Toran's family. Honor: promise Devon, Vividos, Xavier or Kilandra something, then keep your word. Valor: Khumash-
  Gor's tomb, a foe far above your level, or standing with Tenebrae against the dead. Humility: say words for Lothian
  with Vividos, and play with the children of Tenebrae (let them win). Spirituality also grows from the rite.
- Endings: after the final movie, a closing page tells how Pagan remembers the Avatar (Liberator, Pragmatist or One More
  Tyrant, by your virtues) and what became of the people you helped.
- Endings: One More Tyrant now takes real cruelty: killing townsfolk, stealing in front of people, cruel answers, and
  promises you never kept. A quiet game with few good deeds ends as the Pragmatist, and a lot of innocent blood keeps
  even a very good Avatar from the Liberator ending. Some conversations now offer a cruel answer.
- Companions no longer follow you into water, lava or over deadly drops. When you cross stepping stones, jump a gap
  or climb away, they wait at the edge, then join you in a puff of smoke once you stand on safe ground with room.

## 0.19.0 - 2026-11-08
- Debug crit-path guide: where the next target can only be reached through teleporters (Mythran's
  house on the Plateau, Daemon's Crag, the Obsidian Fortress), the guide line now leads to the first
  teleporter, then from where it lands to the next one, then to the target.
- The Grant Peace spell now works: it lays the undead around you to rest, and they give XP as if you had killed them.
- Virtues: the first good deeds count. Grant Peace on the undead raises Spirituality; feeding the Lurker's pet calms it
  (it leaves you alone until hurt) and raises Compassion; telling Rhian the truth about her son raises Honesty, and
  bringing her his ring raises Compassion; Devon can be told that Carthax Lake flows again (Compassion), and why you
  really need the Tear of the Seas (Honesty). Each counts once, and the message log says which virtue grew.
- Controller: with a person picked as the target, A once tells you his name (like a click) and A twice quickly
  talks to him (like a double click). This now works even when he has wandered a step since the first press.
- Character sheets: with Leveling on, every sheet (yours and each companion's) now shows your level in small letters
  under the health and mana bar, with a short XP bar beside it. Click the bar to see your XP and how much the next
  level needs. The level no longer sits in the sheet's top-left corner.
- Virtues: the Avatar's sheet has a small octagon of eight gems in its bottom-left corner, one for each virtue
  (Honesty, Compassion, Valor, Justice, Sacrifice, Honor, Spirituality, Humility), with a ninth gem in the middle for
  the Avatar. A gem starts dim and brightens as its virtue grows, glowing near the top; click one to see its score.
  The new "Virtues" option (Game, default On) turns it off. Nothing earns virtue yet; that comes next.

## 0.18.1 - 2026-11-08
- New creatures: two wraiths in the Pit of the Dead and one Flame of Pyros on the Plane of Fire moved to quiet stretches, away from the monsters already there.

## 0.18.0 - 2026-11-07
- Companions: they now talk to each other on the road. At 33 quiet places along the main roads (from the Tenebrae
  shore and streets to Argentrock, Carthax Lake and Daemon's Crag), two companions walking with you may start a short
  exchange: teasing, grumbling, noticing things about each other. Every pair of the eight has something to say, 65
  exchanges in all; each is heard once per game. Once a pair has run out,
  you may now and then hear them prove it. A place stays quiet for five minutes after it was used, and nobody chats during a
  fight, a conversation or a scene. Lines appear over their heads and in the message log. Works with Leveling on or off.
- New Dialogue: eleven of the cleverest people in the land (Bentic, Darion, Mythran, Vividos, Gorgrond, Beren, Vardion, Bane,
  Torwin, Stellos and Arcadion) have one more question for an Avatar with a sharp mind: it shows in their menu only when your
  Intelligence is high enough, from 18 for Bentic and Darion up to 25 for Arcadion.
- Levels: every kind of monster now has one level of its own (trolls 6, daemons 8, skeletons 2 and so on). Where the
  original game puts a kind early, a weaker one stands there instead, shown as "young troll" or "lesser ghost". The XP a
  monster gives changed to match, so you still reach each level at about the same point in the story.
- New creatures (option "New creatures", on by default; with Leveling on or off): five new foes in the late game, each with
  a weakness to find. The Lurker's pet in Carthax Lake and off Argentrock's beach can only be hit while it is up, and food
  thrown into the water lures it up. The wraith in the Pit of the Dead and the deep catacombs shrugs off spells and plain
  weapons, drains your mana, and fears Ilvaran's Light of the Ancients. The lava wyrm in the lava of Daemon's Crag and the
  lava tunnel can only be hit when it rears, and Endure Heat or Fire Shield keeps off its fire. The storm eye on the Plane of
  Air throws lightning that jumps between people standing close, and goes for metal armour. The Flame of Pyros on the Plane
  of Fire and in the Obsidian Fortress laughs at fire but fears the Tempest spells of Varn and Nerissa.

## 0.17.0 - 2026-11-07
- Levels: companions' spells now come with experience, with Leveling on. Attack spells and the Tempest's spells open up as
  the companion levels up; healing and summoning wait until you have learned a matching spell yourself (Ilvaran heals once you
  carry the Healing Touch or Restoration focus, Morvain raises bones once you know Summon Dead). Ilvaran's and Nerissa's
  raising rites always work.
- Levels: Morvain, Kalvax and Wynna now use up reagents: each spell has a number of uses as well as its mana cost. They start
  well stocked, tell you when they run low and when they run out, and explain it if you ask. Older saves start full.
- Levels: when you finish talking to Mythran, each of them in your party buys what they need with the obsidian in their own
  pack (Necromancy is cheap, Thaumaturgy dear). The message log says what they bought. Drop a pile of obsidian on them to give
  them money; the amount offered first is what they still need.
- Levels: the turn-based spell list shows how many uses are left, and greys out locked spells with the reason. The party
  portraits' tooltips list each companion's spells the same way. Companions save their last few uses for real threats.
- With Leveling off companion magic is exactly as before.

## 0.16.0 - 2026-11-07
- Levels: Darion trains STR or DEX and Mythran now gives lessons for INT, with Leveling on. Each session raises one stat
  by 1, up to 30, and costs more the higher the stat is: 50 obsidian at 10, about 155 at 20, about 415 at 29. You can
  train a companion who is walking with you, too. They pay from their own pack first, then from yours. Darion's old
  sparring session, which no longer raised anything, is gone from his menu.
- Levels: five rare tomes lie in hard-to-reach or late places. Reading one at INT 30 or more gives +2 INT, once per tome.
  Below 30 the words are beyond you for now, and you keep the book.
- Levels: seven rare elixirs of might (+1 STR) and grace (+1 DEX) are hidden around the world, mostly off the beaten path.
- Levels: drop a tome or an elixir on a companion (in the world or on their portrait) and they read or drink it. One used
  from their pack works the same way.
- Levels: three ordeals, hard climbs to high places. Reaching one the first time gives the Avatar +2 STR or DEX, with a
  line in the log and the gain over his head.
- Levels: seven great deeds of the story give each companion walking with you +1 in their main stat (STR for Brannoc and
  Varn, DEX for Corvin, INT for the casters). The Avatar gets nothing from these; he has his level-up point.
- With Leveling off none of this appears. Older saves load; the items appear when you next enter their maps, and story
  deeds already done do not pay.

## 0.15.0 - 2026-11-07
- Levels: stats now matter more, with Leveling on. STR adds a little damage to every blow, monsters' too, and makes the
  Avatar climb faster. The hammer, axe and sabre need STR 15, 14 and 13: below that they still work, but hit weaker and
  slower, and the log tells you when you equip one.
- Levels: DEX keeps its old effects and goes on past 25 with slightly faster attacks. Everyone can now dodge a blow
  ("Dodged" shows over the head and in the combat log): about 1% per 10 DEX, more past 30, at most 20%. In turn-based
  fights a high DEX moves a little further.
- Levels: with enough DEX the Avatar notices a trapped chest when he comes near it, and disarms the trap.
- Levels: INT makes spells hit harder (double at INT 30): companions' spells, monster casters, and the Avatar's Flame
  Bolt, Explosion and Conflagration. Chest traps are not affected. Morvain's skeleton lasts longer with his INT, and now
  fights at his level.
- Levels: every monster has a level from 1 to 8 by where it lives, with its own XP, HP, damage, STR, DEX and INT, so the
  trolls near the start are no longer level 8 brutes. Early monsters are a fair fight for the Avatar alone; from level 4
  one monster is about an even match for two or three of you.
- Levels: companions no longer gain INT by casting when Leveling is on.
- Levels: late-game monsters and story steps (from the Sorcerers' fortress on) give more XP, so level 8 comes near the last chapter;
  the early and middle game keep their pace. Older saves get their level from the same pacing.

## 0.14.0 - 2026-11-07
- Companions: nobody can hurt someone you have not recruited yet, and monsters ignore them.
- Companions: anyone who dies can still be raised where they fell. Once you leave the map, they make their own way home,
  alive and with their things, and can join you again. Each has a few words for you when you find them there.
- Levels: a new "Leveling" option (Game page, on by default). You and your companions now gain up to eight levels from
  kills and from story steps; stats no longer rise by use, and the old limit of 25 is gone. Turned off, the game plays
  by the old rules exactly.
- Levels: each level-up gives a sound and "Level up" over the head. The Avatar also gets one point to place: a small
  prompt waits at the top of the screen until no fight or talk is on (mouse, keys 1 2 3, or L3 on a controller).
- Levels: everyone walking with you earns full XP; a body that rises again pays only once, and monsters far below you pay
  little. Late recruits join one level below you, at most level 5. The numbers are first guesses and will be tuned.
- Levels: the paperdoll and the side panel show level and XP. An older save keeps its stats and starts at the level its
  story progress suggests.

## 0.13.0 - 2026-11-06
- Companions: the eight no longer wait together on the beach. Each now has a place of their own somewhere in
  the world, waiting to be found and talked to; some you can reach early, others much later in the story.
  In a save from before this change, those who never joined have gone to their places; anyone who joined stays put.
- Companions: one who parts ways now stays where you left them instead of walking back to the beach.
- Debug Mode: while it is on, everyone not yet recruited waits on the beach as before (and goes back to their own
  place when it goes off), and the debug menu's "Where they are..." lists where each companion is.
- Zooming out now stops at about twice the normal view (at a 3x window, one and a half times). Debug Mode
  still zooms out all the way.
- U8 Ether Gate: the project's public name. It is an engine for Ultima VIII: Pagan and needs your own copy
  of the game. Licence: GNU GPL v3 or later.
- A release package: source, a web build for any static host (it asks for your game folder and keeps the
  files in your browser; nothing is uploaded) and a Linux build. No game files are in any of them.
- Works with whatever copy you have: the game says which copy it found (Help, About page). Fixes and
  additions made for other data switch themselves off with a line in the log, and missing music, sound,
  movies or speech are simply skipped. Only English v2.10 has been tested.

## 0.12.0 - 2026-11-06
- Combat: when you have moved and nothing useful is left to do (no foe in reach, no spell you can cast
  at anyone, no item worth using), your turn ends by itself and the character defends.
- Combat: the arrow keys and the D-pad now move through the combat icons properly: left and right along
  a row, up and down to the other row in the same column. The tenth icon, Party Auto, can be reached too.
- Combat: A (or Enter) on a target fires the spell or attack at once, and it also works on the target
  cursor of a focus or the Skull of Quakes, where the arrows and D-pad pick among the fighters.
- Combat: a target out of range shows a tiny "out of range" above it, and pressing on it costs nothing.
- Combat: choosing a spell shows a very light dotted circle round the caster for how far it reaches.
- Companion spells reach twice as far, and Fire Field covers twice the radius. The caster of an area spell
  is no longer hurt by their own spell.
- Combat: a turn still running after 15 seconds ends with "(name) missed its turn", so a stuck enemy can't
  hold up the fight.
- Wynna's Lightning now strikes from the sky: a jagged bolt for a few frames, then the hit, the thunder and
  a one-frame flash of the game picture.
- Winning or escaping a fight now ends combat mode: weapon away, normal stance, normal music, and the side
  panel back to portraits and messages. Real-time combat does the same once all foes around are gone.
- NPCs and companions mutter idle remarks about a fifth as often. Conversations, scenes and quest lines
  are untouched.
- The Pit of the Dead: the old caved-in cave mouth in the west of the Pit is now properly buried under a heap of rubble. There is no way through it yet. Options, Game, "Underworld entrance" turns it off.
- Easy Ledges no longer lifts you two storeys: the highest ledge you can now reach is what a full climb
  reaches (72 units, about the original game's limit). Snapping to a ledge from a distance or at an angle
  stays as easy as before, and catching a ledge as you fall from above still works from any height.

## 0.11.0 - 2026-11-06
- The Skull of Quakes now has its power: use it anywhere but on its red slot (pick anything with the
  cursor) and the earth shakes, hurting the hostile creatures around you, never your party or
  townsfolk. It then stays quiet for 30 minutes of play (look at it to see the time left); in a
  turn-based fight it is a Use item action. Its old use on the slot in the Upper Catacombs works as
  before. Option "Item powers" (Game) turns this off.
- From the official v2.12 patch: F7 quick-saves to a slot of its own and F8 loads it (controller: LB + D-pad
  Up / Down); it also appears on the diary's autosave page. R uses the recall item, like B the bedroll and K the
  keyring (controller: LB + Y, B, X). Roof hiding has moved from R to Shift+R.
- v2.12 changes (option in Game, On by default): the recall item now works everywhere except the Obsidian Fortress
  and the endgame planes; reagents and other stacks you put into a bag or backpack join the stack already there
  (the dead man's elbow stays apart from wooden sticks); with Classic controls, both mouse buttons jump to the spot
  you point at if it is ground in reach, otherwise he shakes his head.
- Responsive controls: every jump now goes forward with the landing help, both mouse buttons included.
- Restored cut content (option "Undead invasion" in Game, On by default): once you take the Heart of Earth, the dead
  walk Tenebrae. Ghouls replace the town guards, peasants stay indoors, the ground shakes now and then, and many
  townsfolk (Darion's shop and Jenna's bar included) will not talk until Lithos is dealt with. Nothing you still need
  for the main story is lost; switching the option off ends it at once.
- The help page (F1) now turns over as many pages as it needs instead of running off the bottom.
- New dialogue for Gwillim the herdsman and Beren the sorcerer: as the story moves on, a new answer
  appears in their own conversation ("Mordea", "Hydros", "The flooded Mouth"...), replaced by the
  newest news; each has its own replies and returns to their usual talk. Option "New Dialogue"
  (Game) turns it off.
- Fixed: Beren at the Mouth of the Daemon said only his parting warning if you had met him before
  (or apologised to him); he now greets you, explains the cavern and tells you where to go.
- Fixed (original game bug): the Spell of Resurrection book in the Argentrock monastery showed the
  text of the Intervention book; it now shows its own.
- Fixed (original game bug): Vividos takes any executioner's hood (they all look alike), and his
  dead man's elbow no longer disappears into a pile of ordinary wooden sticks when you drop it there
  (or Sort the bag): it keeps a stack of its own.
- Fixed (original game bug): the Ancient Necromancers' test can no longer be skipped. A Necromancy
  spell they teach can only be made in a bag once that Necromancer has taught it (until then:
  "Something's not right."), and they never talk without Death Speak, even after the spell was cut
  short by leaving the map.
- Fixed (original game bug): the blackrock pieces absorb Lithos, Stratos and Pyros only on their own
  Planes (Earth, Air and Fire), as the Tear of the Seas already did with Hydros. Before, Lithos could
  be absorbed in the Hall of the Mountain King before you had even met him, and Stratos on Argentrock
  at any time, which sent you to the ethereal plane too early (a frozen game or a dead end).
- Fixed (original game bug): Gwillim the herdsman had nothing to say after your first visit but
  "Farewell"; now "Hello, my friend." leads on to all his usual topics again.
- Fixed: 80 spelling mistakes in the game's books and conversations ("Restortation", "Etherial
  Travel", "cemetary", "existance", "Order of Enlightment", "it's location"...).
- Fixed (original game bug): you can no longer fall out of the world. If the Avatar walks off the edge
  of an area or drops through a hole into empty space, he is put back where he last stood, unhurt,
  instead of being stuck down there (or killed by the fall); and he can no longer walk off the edge
  of the map. In the stitched world, walking off Tenebrae's west edge now takes you down onto the
  shore beyond instead of into nothing.
- Fixed (original game bug): the Tenebrae gate guard no longer spins round while he walks up to you.
- Fixed (original game bug): creatures made with the Summon Creature spell behave like every other
  summoned monster (they answer when looked at and vanish once you are far away).
- Fixed (original game bug): the moss curtains in the catacombs no longer block like walls: you can
  reach the bodies in the alcoves behind them.
- Fixed (original game bug): six more missing floor and lava tiles in the Ancient Necromancers'
  caves that you could fall through, and a gap in the ground of Herdsman's Valley.
- The wind on the Tenebrae dock and the dripping water on the Plateau and in the Hall of the
  Mountain King can be heard again.
- All of these follow the "Original bug fixes" option (Game); Off plays the original text and scripts.

## 0.10.1 - 2026-11-06
- Editor: two restore points. "Pagan" is where the original Ultima 8 had each segment; "Baseline"
  is the world layout as it stands on 6 November (places, layers, heights, seams and links).
  Layers panel: "Reset to Pagan" / "Reset to Baseline" for the selected segments, "Reset all to
  Pagan" / "Reset all to Baseline"; all undoable. Select several segments with Shift+click (on the
  map or in the Layers list) or by dragging a box over empty ground. "Show Pagan spots" and "Show
  Baseline spots" preview every segment there (view only; they replace "Original spots"). The
  original maps' contents are never edited (painted areas are separate), so there is nothing to
  revert inside a segment.

## 0.10.0 - 2026-11-06
- New side panel in wide windows: the game picture moves to the left and the spare space on the
  right shows your party's portraits in one row (click one for that member's inventory, or drop an
  item on it to put it in their pack) and a message log of what you looked at ("You see fish."),
  what people said to you and the answers you picked, and other messages. Scroll it with the wheel,
  its bar, or the controller (press the right stick to move to the panel). In narrow (4:3) windows
  the picture shrinks a little to make room. Options, Display: "HUD: Side panel / Classic".
- In combat the panel shows each party member's portrait, name and hit points (whose turn it is is
  highlighted), the monster acting now, buttons with icons and tooltips for every turn-based
  action, and a combat log of who hit whom for how much, misses, blocks, spells, heals and deaths.
- While Party Auto is on, an "Auto mode: ON" button sits at the bottom left of the picture: click it,
  press Esc or the controller's B to give the whole party back to you.
- Turn-based Move now reaches almost four times as far (300 instead of 80). Instead of the dotted
  area there is a destination marker with the route to it; the arrow keys or the controller push it
  and it stops at the edge of the range (the mouse picks the nearest spot in range). B or Esc goes
  back one step: first the picked spot, then the action menu.
- The controller's LB and RB now turn the pages of every book: the options book, Read Diary and
  Write Diary, and books and scrolls found in the world (PgUp / PgDn on the keyboard too). At the
  last page of a book RB stays there instead of closing it.
- The page corner for turning to the next page now sits on the right-hand page's top corner, the
  mirror image of the left one (it used to hang off the page over the book's edge, curling the
  wrong way); clicking where it is drawn turns the page with one click.
- Write Diary opens on the first free entry after your most recent one, ready to type (it could
  open on the last page after an autosave); Read Diary opens on your most recent save.
- A fight that starts in the middle of a conversation (a monster attacks, or you draw your weapon)
  now ends the conversation at once, so the fight can go on and the Avatar is not left frozen.
- The stitched world is laid out like the whole island: Herdsman's Valley sits right north of
  Tenebrae (walk out of the north gate and you are in it), the Cemetery joins the far end of the
  East Road, and the Plateau, Stone Cove, Carthax Lake, Argentrock Isle and Daemon's Crag are where
  the island map has them. Caves, catacombs and dungeons lie on the layers below, the planes above;
  no two places overlap on a layer any more. Unused test maps wait in an "Unplaced" row off the
  south coast.
- World map (M): shows the new layout, one layer at a time (Page Up / Page Down).
- Editor: a Layers panel (Y or the Layers button) lists every layer with its name, height and
  what is on it, lets you hide or show a layer, and pick any map group or painted area; the
  current layer is named in a strip at the top, and places on other layers show as dashed ghosts
  marked ABOVE or BELOW. Move a group by dragging (Move tool), with Ctrl+arrows or the panel's
  buttons, and change its layer with Ctrl+PgUp / PgDn; groups joined by walkable seams move together.
- Editor: Undo and Redo (buttons, Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z) cover group moves and layer
  changes as well as painting and area changes; the buttons say what they will undo.
- Editor: an old saved world layout (from before the layers) is replaced by the new one when the
  editor opens; your painted areas are kept.
- Editor: every segment (each original map, and each painted area) is drawn whole, with all of
  its original items; where two segments overlap both are drawn (the selected one on top) and
  their outlines turn red ("OVERLAP"), nothing is hidden. Nothing in the original maps is changed.
- Editor: the new Segments tool (the default, S; Alt+drag with any tool) grabs any segment under
  the pointer and drags it; the segment under the pointer is outlined and named. Only that segment
  moves; "Joined" (J) moves pieces joined by a walkable seam together. Pulling a segment off a
  seam breaks the seam (its crossing becomes a teleport again); bringing it back snaps onto the
  seam and joins it again.
- Editor: segments snap to a grid (512, 256, 128, 64 or off: N or the Snap button; the grid is
  drawn) and to the exact spot where a seam or a teleporter lines up when they come near
  (hold Shift for no snapping to those).
- Editor: Reset a segment, or all of them, to its original atlas spot, to layout revision 0 or to
  the shipped layout (Layers panel; undoable), and "Original spots" shows every segment where the
  original atlas had it. Overlaps you make are allowed and marked; the game still loads them.

## 0.9.0 - 2026-11-06
- Conversations on the controller: B now picks the menu's way out, "back" (or "Never mind", "Enough
  about ...") in a sub-menu, else "Goodbye" / "bye" (a small "B: ..." hint shows which); the stick and
  the D-pad move the highlight to the answer above, below, left or right as they are laid out, and
  repeat when held. The arrow keys do the same, and Enter then answers the highlighted one.
- Nerissa has a new portrait in the party bar, made from a detailed painting of her (her slim face,
  grey-teal eyes and long dark wavy hair), and the same face and hair on her paperdoll figure, whose
  hair is now as dark as in the portrait.
- A start menu: when you have saved games (or autosaves), the game no longer plays the intro at
  every start. It opens the main menu over your most recent save, with "Continue" (Enter) and
  "New Game" (N, plays the intro) above the usual entries. Esc also continues. New option "On Start:
  Main menu / Continue / Intro" (Game). The first start, with no saves, still plays the intro.
- The main menu reacts to a single click at once (it used to need a double-click or several
  clicks). Book and scroll pages also turn again on a single click.
- The main menu and the diary work with the keys and the controller: Up / Down (D-pad) move the
  highlight, Enter (A) chooses; in the diary Left / Right turn the pages.
- The game has its own icon, a gold "U8" on a dark red roundel, for the window and the browser tab.
## 0.8.0 - 2026-11-05
- Ledges are much easier to climb (Responsive controls, new option "Easy Ledges", on): hold jump
  (Space, X on the controller, or both mouse buttons) anywhere near a ledge and the Avatar walks to
  it, lines up, and climbs up or jumps, grabs the edge and pulls himself up, up to about two body
  heights. He also catches ledges as he falls past them. A small yellow mark shows the ledge he is
  going for; "No ledge in reach" says when there is none. Roofs and wall tops are not ledges.
- Debug mode: the critical-path guide line now follows the real walking route (round walls, up the
  stairs, through doors) instead of pointing straight at the target, with up / down marks on the way,
  the walking distance left, and a hint such as "Walk 3321: up the stairs". Debug menu: "Crit path:
  route line / straight line".
- Nerissa has her own look: long dark hair, a sea-foam gown with a pale sash and deep-blue hem,
  bare calves and blue slippers, in every move (walking, running, casting, fighting, falling) and
  on her paperdoll and party-bar portrait. No other character changes.

## 0.7.0 - 2026-11-05
- Companions: Brannoc and Corvin now land their sword and axe blows (Brannoc wears plate armour now).
- The stitched world (new, on by default): Tenebrae's three quarters, the docks, Fisherman's Reef,
  the East Road and the lower catacombs now join up. Walk from one into the next with no loading
  jump, and see the next area across the border before you get there.
- The world is laid out as one place: caves and catacombs lie below the surface, the planes and
  high places above. Doors and portals between levels still work as teleports, as before.
- Options: "World: Stitched / Original maps" (Game). Original maps plays exactly as the original
  game did; saves made in either setting load in both.
- The world map (M) shows the stitched layout one level at a time (Page Up / Page Down), with the
  outlines of the levels above and below.

## 0.6.0 - 2026-11-05
- Map dresser (new): painted areas are turned into real map pieces learned from the original maps:
  varied floors, walls with corners and working doors, shores, ledges, roofs, and points of
  interest chosen from your note (camps, shrines, graves, tables, ...). Seeded: re-roll for a new look.
- World editor (new; `?edit`, or "Editor" in the debug menu): paint new areas beside the existing
  maps (floor, walls, doors, passages, water, chasms, stairs, heights) with brushes, lines,
  rectangles and fill, add notes to points of interest, put areas on layers above or below, undo
  and redo. Your areas are kept in this browser, with Download / Upload as a backup.
- Turn-based combat (new, on by default; Options > Controls > Combat switches back to real time):
  when a fight starts time stops and everyone takes turns by Dexterity. On your turn pick Move
  (the reachable ground lights up, the route is shown, click again to go), Attack, Cast, Use item,
  Defend, Wait, Auto or End turn, with the mouse, the keys or a controller.
- Party Auto (the A key, the RB button or the button under the turn list) lets the whole party
  fight by itself; hold Space, A or the mouse button to speed up the enemies' turns.
- Fights end in victory, when you get away, or as before when the Avatar falls. Story fights with
  scripted characters stay in real time. You can save in the middle of a fight.
- Entering combat brings companions who were left behind to the edge of the screen beside you.
- Controller: in a conversation A again picks the highlighted answer (the D-pad moves the
  highlight); in books, the options, the quit box and other menus A activates what is highlighted.
- Controller: A now works like the left mouse button on the highlighted target or the pointer
  (tap to look, tap again to use, hold to drag), X jumps, B closes the window under the pointer
  or the newest one. LB with the D-pad picks a companion on the party bar.
- A controller only starts acting after you press one of its buttons.
- Zoom out much further (down to about 1/16 or 1/32 of normal size); far views show the map with
  everyone moving on it.
- Dragging items works at every zoom level (the bedroll into the backpack, items onto the Avatar).
- Dropping is easier: anywhere on a backpack or bag window puts the item in it; on the paperdoll
  outside the figure, or on a companion, puts it in that backpack.
- Clicking through speech no longer skips two lines at once: each new line and answer menu ignores
  clicks for half a second; holding the button still fast-forwards.
- Jumps always go forward (Responsive), and land on a nearby safe platform instead of in the water
  when there is one.
- Hold jump to grab a ledge and climb up, even one a little too high to climb directly, or while
  falling past it.
- The Avatar no longer walks into water, lava or deadly drops by accident (Responsive), and is
  lined up with doorways and narrow paths when he pushes against them.
- Open doors no longer get in the way, and doors never close on anyone (Responsive).
- F1 (or holding Back) shows every control; M or Tab opens the world map from the game.

## 0.5.0 - 2026-11-05
- The options page is laid out to fit the book: every setting (Debug Mode too) in groups, smaller
  text that stays on the pages, more pages when needed; mouse, keys and controller all work.
- Sound effects, music and the movies (intro, endgame), with a prompt to skip a movie.
- The end credits and the quotes.
- The options page works and remembers your settings (sound, music, text speed and more).
- Controls: choose Responsive (modern) or Classic (like the original).
- Smooth scrolling camera, and live zoom in and out of the world.
- Game controller support (early version; more controller changes are coming).
- The world atlas from inside the game (M or Tab).
- F1 shows the keys and controls.
- Click (or Enter) to move through dialogue and scenes faster.
- Movement assist: the Avatar steps around small obstacles and lines up with doors.
- Doors open more reliably.
- Sort the backpack.
- A debug mode (F10) with the critical path through the game: a guide line to your next goal, with an up/down arrow when it is above or below you; long hints wrap instead of being cut off.
- Autosave after every major story step (three rotating saves, shown first in the diary); the
  death menu offers "Load last autosave". Turn it off in the options.
- Companions: eight new characters wait down the beach east of Devon's camp. Talk to them, take up
  to six along; they follow you everywhere, fight beside you, heal, pick locks, carry gear and
  have their own paperdoll. A party bar (P) shows them. Turn them off in the options.

## 0.4.0
- Inventory: open the backpack and other containers, drag items between them, the world and
  the Avatar; split stacks with a slider.
- The paperdoll: equip weapons and armour, see your stats.
- The main menu (Esc), with the diary, options and quit.
- Name your Avatar at the start of a new game.
- Save and load games in the diary; saves are kept in your browser.
- Tenebrae: through the gate after the execution scene.

## 0.3.0
- Walk, run and take careful steps with the mouse or the arrow keys.
- Jump, climb, and hang from ledges.
- Physics: falling, gravity, items that drop and land.
- Open and close doors.
- Combat: draw your weapon, attack, block and kick.

## 0.2.1
- Zoom out of the map, all the way to a view of a whole map.
- The world atlas: all the maps of Pagan on one page.

## 0.2.0
- The opening plays: the game's own scripts run, the Avatar arrives in Pagan,
  and Devon talks to you.

## 0.1.0
- The map viewer: look around every map of Pagan.
