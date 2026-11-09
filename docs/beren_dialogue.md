# Beren: new dialogue and the Mouth of the Daemon fix (engine-side, our own writing)

Beren (NPC 32, usecode class BEREN) is an Acolyte Sorcerer of the Cabal, living in the Sorcerers'
Enclave in Daemon's Crag (map 12). Bane is his mentor (and, by the way he speaks of her, more); he
scorns Vardion ("a petty overlord") and Gorgrond, and is the sorcerer the people of Tenebrae call on
when they are robbed or assaulted (their "Help me, Beren!"; he then appears and passes sentence). He
is courteous and dry while respected, and quick to take offence. The Enclave is reached only after
the lava river cools (Hydros' water through the Mouth of the Daemon, G[52:2]), and on the way in he
meets the Avatar in a short scene at the Mouth (egg class BERENHCH, map 25).

## What was wrong (reproduced in the engine)

The Mouth scene has lines for two states only: a stranger (G[392:2] = 0: his welcome, the four
replies, the hint about crossing to the ledge where he stands and seeking out Bane, then his parting
warning) and an angry Beren (G[396:2] non-zero: the hostile version and the warning). When the
Avatar has met him before and he is not angry (G[392:2] non-zero and G[396:2] = 0, which is also what
an accepted apology leaves: "please forgive me" sets G[396:2] back to 0), the script skips straight to
the parting warning: one line, "as if continuing an earlier conversation", and he is gone; the
explanation and the hint toward Bane are lost. This matches the community reports ("most of Beren's
dialogue lines vanish after you partly cool the lava river"; "never talk to him, or anger him and
refuse the apology" avoids it). The engine test `beren_mouth` shows the three states: 4 lines, 3
lines, and 1 line.

**The fix** is a scene patch (`scenes` in `data/dialog/beren.json`): in that state, when the scene's
script makes Beren speak for the first time, our lines and three replies come first (he recognises
the Avatar, explains the cavern and gives the same hint), then his original warning, then the script
goes on (stasis lifted, he returns to the Enclave). Nothing in the story changes. The patch is a bug fix, so it follows the
"Original bug fixes" option (on by default), not "New Dialogue"; with it off the original behaviour is kept. No unwinnable bug in Beren's conversation was found in the
sources (Ultima Codex "Bugs", "Beren", "Mouth of the Daemon" and its talk page): the reports describe
the lost lines only. His Enclave conversation itself (every state of G[392], G[395], G[396], G[398],
G[142] tried) keeps its menus in the engine.

## New topics

At points in the story an answer appears in his own menu (the first menu that has "Farewell.", so
never on his "your name?" question), labelled by its subject; a newer one replaces the older in its
slot. Two slots: his own life (duties elsewhere, opinions of Tenebrae folk, the town's news, his
service once the Avatar is Master) and the world (the cooled Mouth, the trials, the ceremony,
Malchir, the blackrock, the Titans). They are hidden while he is angry with the Avatar (G[396:2]) or
grieving for Bane (G[142] bit 0: his "you allowed Vardion to destroy..." state). The townsfolk he
speaks of are ones everyone in Tenebrae knows (Korick, Bentic, Devon, Mordea): no reliable
"met" flag was found for them.

Source: `engine/data/dialog/beren.json` (regenerate this file with
`python3 -I tools/dialog_doc.py data/dialog/beren.json docs/beren_dialogue.md INTRO`).

## Stages

The current stage is the last one whose condition holds (critical-path step ids from `engine/data/critpath.json`; a step also counts as done when a later milestone is).

| Stage | Meaning | When |
| --- | --- | --- |
| `enclave` | Reached the Enclave (the lava river cooled) | after `to_daemons_crag` |
| `disciple` | A disciple of Bane or Vardion | after `choose_mentor` |
| `trials` | Passed the first test | after `first_test` |
| `fortress` | Through the Obsidian Fortress | after `arcadion_done` |
| `ceremony` | After the Pyros ceremony | after `pyros_ceremony` |
| `master` | Malchir is dead | after `kill_malchir` |
| `blackrock` | Gathering the blackrock | after `show_tongue` |
| `ether` | Beyond the world | after `ethereal_travel` |

## Topics (in story order)

Each top-level topic is an answer in Beren's own menu. Topics in the same slot replace each other (the last one below whose condition holds is shown); at most 2 slot(s) at a time. Inside a topic "Something else." goes back to his own menu ("Back." from a deeper menu).


### Slot: beren

- **Your duties** (after `to_daemons_crag`; until `choose_mentor`; condition `[{"global": [396, 2], "eq": 0}, {"global": [142, 1], "eq": 0}]`)
  > My duties? Study, chiefly. But the Cabal lends me out, now and then, to places that need a sorcerer's hand.
  > You may find this room empty some days. Do not take it as an invitation to look through my things.
  - *Avatar:* "Where do they send you?"
    > Across the water, where people are many and patience is short. When they are wronged, they call. I answer.
  - *Avatar:* "Who calls you?"
    > Those who have been robbed or struck. Pray you never hear one of them call my name on your account.
  - *Avatar:* "Do you like the work?"
    > I like order. The work keeps it. Whether I like the people I keep it for is another matter.
- **The townsfolk** (after `choose_mentor`; until `trial`; condition `[{"global": [396, 2], "eq": 0}, {"global": [142, 1], "eq": 0}]`)
  > Now that you are one of us, I may speak plainly. I spend more of my days in Tenebrae than I would like.
  > Some of its people are worth the trouble. Some are not. Ask, if you have met them.
  - *Avatar:* "Korick the smith"
    > A strong arm and a short temper. He once dealt with a brawler himself before I could arrive.
    > I should be annoyed. I find that I am mostly grateful. Do not tell him.
  - *Avatar:* "Bentic the librarian"
    > The librarian. He will tell you the history of a doorknob if you stand still long enough.
    > Still, he knows where every book is kept. That is rarer than wisdom, and more useful.
  - *Avatar:* "Devon the fisherman"
    > An honest man, I am told, which in Tenebrae makes him nearly a curiosity. The guards watch him closely.
  - *Avatar:* "Lady Mordea"
    > The Tempest rules the town; the Cabal keeps its own counsel. We get on by not asking each other questions.
    > I would advise you to follow the same rule. She does not care for curious strangers.
- **Tenebrae** (after `trial`; until `kill_malchir`; condition `[{"global": [396, 2], "eq": 0}, {"global": [142, 1], "eq": 0}]`)
  > So the Tempest has fallen, and a fisherman sits in her palace. The town talks of nothing else.
  > My work there will not change. Thieves do not care who wears the crown.
  - *Avatar:* "Devon is a good man."
    > Then he will find the palace a lonely place. I will do him the courtesy of keeping his streets quiet.
  - *Avatar:* "And Korick?"
    > Hammering, as ever. He forged half the new guards' blades in a week. He seems almost cheerful.
- **The Cabal now** (after `kill_malchir`; condition `[{"global": [396, 2], "eq": 0}, {"global": [142, 1], "eq": 0}]`)
  > Master. Forgive me for speaking first. The Cabal is uneasy, and someone should tell you so.
  > I will keep the town across the water in order, as before, unless you command otherwise.
  - *Avatar:* "Carry on as before."
    > As you wish. It is good to serve someone who knows what he wants.
  - *Avatar:* "How is Bane?" (condition `{"not": {"npc_dead": 29}}`)
    > Watching you, as am I. She says you have the makings of a legend. I have stopped arguing with her.

### Slot: world

- **The flooded Mouth** (after `to_daemons_crag`; until `first_test`; condition `[{"global": [396, 2], "eq": 0}, {"global": [142, 1], "eq": 0}]`)
  > Since the lake overflowed, the Mouth hisses like a kettle day and night. Half the river is black stone now.
  > For generations that fire kept out anyone not worthy of the Enclave. Now a determined goat could walk in.
  - *Avatar:* "That was my doing."
    > I had guessed as much. Few would think to drown a volcano. It was clever. It was also extremely rude.
  - *Avatar:* "Will the fire return?"
    > Pyros is patient. The rock will crack, the fire will rise. But not in my lifetime, I suspect, nor yours.
  - *Avatar:* "What do the others say?"
    > Vardion rages, which pleases me. Gorgrond asked whether the steam was a spell. Bane laughed.
    > When Bane laughs, I have learned not to worry.
- **The trials** (after `first_test`; until `pyros_ceremony`; condition `[{"global": [396, 2], "eq": 0}, {"global": [142, 1], "eq": 0}]`)
  > You have passed your first test. Do not let it go to your head; the later ones have killed better students.
  > Rest before each. Think before you touch anything. That is all the advice the Cabal allows me to give.
  - *Avatar:* "And the Mouth?"
    > Still steaming. The new acolytes walk in over wet stone. Bane says it teaches them humility.
- **The ceremony** (after `pyros_ceremony`; until `kill_malchir`; condition `[{"global": [396, 2], "eq": 0}, {"global": [142, 1], "eq": 0}]`)
  > I have stood at the rites before, but never felt the Lord of Flame so close. The floor was hot through my boots.
  > You were there, at the heart of it. I will not ask what you saw. I am not certain I want to know.
- **Malchir** (after `kill_malchir`; until `show_tongue`; condition `[{"global": [396, 2], "eq": 0}, {"global": [142, 1], "eq": 0}]`)
  > The old Master is gone, and the Enclave is very quiet. Even the lava seems to be holding its breath.
  > He was seldom here, and he led us poorly. Still, I find I do not quite know what to do with the silence.
  - *Avatar:* "He had to fall."
    > Perhaps. Power that sits still goes bad. You at least seem to be going somewhere with it.
- **The black stone** (after `show_tongue`; condition `[{"global": [396, 2], "eq": 0}, {"global": [142, 1], "eq": 0}]`)
  > You carry the Tongue of Flame about as if it were a trinket. It belongs to the Lord of Flame, Master.
  > Whatever you mean to do with it, do it quickly. Things like that do not like to be kept waiting.
- **The Titans** (after `ethereal_travel`; condition `[{"global": [396, 2], "eq": 0}, {"global": [142, 1], "eq": 0}]`)
  > The lava under the Enclave has gone dull, as if something far away has stopped listening.
  > If that is your doing, I will not pretend to understand it. I only hope you know where you are going.

## Scene patches (fixes to original scenes)

### mouth_known (usecode class BERENHCH; condition `[{"global": [392, 2], "ne": 0}, {"global": [396, 2], "eq": 0}]`)

Data fix: the Mouth of the Daemon scene (egg class BERENHCH, map 25). When the Avatar has met Beren before (G[392:2] non-zero) and he is not angry (G[396:2] = 0; an apology sets it back to 0), the original script skips straight to its parting warning: there are lines only for a stranger and for an angry Beren. These lines fill that state. from_ip: only when his first line comes from the scene's closing part (offset 0x092A on, specs/world/bugfixes.md 6), so if a usecode fix ever sends that state to the stranger's speech instead, this patch stays out of the way.

Said first, before the script's own line:

> Well. You again. I wondered who could be fool enough, or clever enough, to send a lake through the Mouth of the Daemon.
> This cavern was the Cabal's first test. Fire kept out the unworthy. Your water has made it a stroll.

  - *Avatar:* "It was my doing."
    > So I assumed. I will not report it. The Master may think a newcomer who can do this is worth a look.
    > There is still one stretch the water did not cool. Find a way across to where I stand and the Enclave is beyond. Ask for Bane there.
  - *Avatar:* "Who could have done it?"
    > Spare me. We have spoken before, and I know that look. Never mind; it is done.
    > If you can cross the last of the fire to where I stand, the Enclave is beyond. Seek out Bane. She will hear you.
  - *Avatar:* "How do I get in?"
    > Most of the river is stone now. The last stretch is not. You will need more than boots to cross it.
    > Reach the place where I stand and you will find our home. Ask for Bane. She teaches those worth teaching.

Then the original line follows and the scene goes on as before.

