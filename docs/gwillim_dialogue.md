# Gwillim: new dialogue (engine-side, our own writing)

Gwillim (NPC 14, usecode class GWILLIM) is the torax herdsman of **Herdsman's Valley** (map 7), the
farm on the trail up to the plateau. He is not a scholar: he is Corinth's husband and the father of
Cyrrus, who left to become a Theurgist healer on Argentrock Isle. In his own conversation (the
original usecode, played in the engine) he is warm and glad of company, proud of his flock and his
son, quick to call himself "law abiding" under Lady Mordea, sure that the Titans are the only real
magic (Mythran is "an eccentric old man"), and touchy about the valley's ghost, which killed his
brother. The new lines keep that voice and stay out of the ghost story.

**How it plays.** At points in the story a new answer appears in his own first menu with a goodbye (before
"Farewell."), labelled by its subject: "The execution", "Mordea", "Hydros"... A newer subject
replaces the older one in its slot. There are two slots: the news of the world, and his son. Picking
one makes him talk (his speech bubbles, logged in the side panel like his own lines); its
sub-answers are the answer menu; "Something else." returns to his own menu exactly as it was, and
"Farewell." there ends the talk as usual. The same answers also appear on a menu that holds only
"Farewell.". He never mentions what the Avatar has not done yet; a few topics give a gentle push
toward the next goal (the learned men in town, the plateau trail, the healers' house).

Source: `engine/data/dialog/gwillim.json` (edit it there; this file is regenerated with
`python3 -I tools/dialog_doc.py data/dialog/gwillim.json docs/gwillim_dialogue.md INTRO`).

## Stages

The current stage is the last one whose condition holds (critical-path step ids from `engine/data/critpath.json`; a step also counts as done when a later milestone is).

| Stage | Meaning | When |
| --- | --- | --- |
| `arrived` | Just arrived | always |
| `execution` | After the execution | after `execution` |
| `seek_mythran` | Told to seek Mythran | after `ask_bentic` |
| `mythran` | Met Mythran | after `meet_mythran` |
| `necromancers` | Among the Necromancers | after `to_cemetery` |
| `lithos` | Met Lithos | after `meet_lithos` |
| `mordea` | Mordea has fallen | after `trial` |
| `lothian` | Lothian laid to rest | after `inter_lothian` |
| `pilgrimage` | The Zealan shrine | after `ancient_ones` |
| `titan_tasks` | The Theurgists, Hydros, the Sorcerers | after `to_argentrock` |
| `blackrock` | Gathering the blackrock | after `show_tongue` |
| `ether` | Beyond the world | after `ethereal_travel` |

## Topics (in story order)

Each top-level topic is an answer in Gwillim's own menu. Topics in the same slot replace each other (the last one below whose condition holds is shown); at most 2 slot(s) at a time. Inside a topic "Something else." goes back to his own menu ("Back." from a deeper menu).


### Slot: news

- **The execution** (after `execution`; until `ask_bentic`)
  > So you were at the docks for the hanging. Corinth heard of it from a drover before the dust had settled.
  > I keep away from such things. A man tends his flock, pays what is owed, and the Lady's justice passes him by.
  - *Avatar:* "Are you afraid of her?"
    > Afraid? I am careful. There is a difference, friend, though I could not tell you where it lies today.
  - *Avatar:* "Where should I go?"
    > I am a herdsman, not a sage. In town there are men who live among books. Ask the learned ones what you need to know.
- **The plateau path** (after `ask_bentic`; until `meet_mythran`)
  > You have the look of someone bound uphill. Most who come this far are going to see the old hermit.
  > The trail past our house climbs all the way. Keep to it and mind your footing where the stones are loose.
  - *Avatar:* "Is the way safe?"
    > Safer than town on a hanging day. There are beasts about, but nothing a man in armour like yours should fear.
  - *Avatar:* "What is he like?"
    > Odd. Polite enough. He keeps a house full of things no sensible man would buy. But he has never done us harm.
- **Mythran** (after `meet_mythran`)
  > Back from the plateau, are you? Did the old man try to sell you one of his scrolls?
  > Do not let him take all your coin. Paper and ink, that is all they are. The Titans hold the only true power.
  - *Avatar:* "His magic is real."
    > So you say. I have watched a storm flatten my barley. That is power. A scroll is a scroll.
    - *instead, after `meet_lithos`:*
      > After all you have told me, I am less sure than I was. But I will not buy one, all the same.
  - *Avatar:* "He spoke well of you."
    > Did he? Well. He always nods to Corinth on the road. Perhaps I have been hard on him.
- **The Necromancers** (after `to_cemetery`; until `meet_lithos`)
  > You smell of grave dirt, friend. Have you been out among the Necromancers?
  > They serve Lithos and keep the dead. We leave them be and they leave us be. That is how it has always been.
  - *instead, after `apprentice`:*
    > Word is that you keep company with old Vividos and his people now. Out by the graves.
    > I will not judge you for it. Someone must see the dead into the earth. Only wash before you sit at Corinth's table.
  - *Avatar:* "Do you fear them?"
    > Fear the ones who bury us? No. I would sooner fear the day they stop.
  - *Avatar:* "What of Lithos?"
    > The Mountain King. Every furrow I plough is his. I give him thanks at sowing and say little else to him.
- **The Mountain King** (after `meet_lithos`)
  > The torax would not settle last night. They stood with their heads low, as if the hills were talking under them.
  > Corinth says it is nothing. But you have been deep under the hills of late, I think. Is it nothing?
  - *Avatar:* "I met Lithos."
    > You... spoke with the Titan of Earth? And he let you walk away?
    > Then you are either a great man or a great liar, and I cannot tell which. Sit down. I will fetch the ale.
  - *Avatar:* "It is nothing."
    > Then I will tell the torax so. They will not believe me either.
- **Mordea** (after `trial`)
  > Is it true? A drover swears Mordea is cast down and young Devon sits in the palace now.
  > I told Corinth to keep her voice low. Twenty years of keeping your voice low is a hard habit to lose.
  - *Avatar:* "It is true."
    > Then I can say it. She was a hard ruler, and I praised her laws because I had a family to keep safe.
    > Corinth danced in the kitchen. Do not tell anyone I told you that.
  - *Avatar:* "What of Devon?"
    > A fisherman on the throne. Well, he knows what a hard day's work is. That is more than most rulers can say.
- **The cemetery** (after `inter_lothian`)
  > Shaana passed by from town. She says the Necromancers have laid their old master in the earth at last.
  > Strange. Even up here the nights feel quieter. Perhaps the dead sleep better when their own are at rest.
  - *Avatar:* "I helped them."
    > Did you now? There is nothing you will not put your hand to, is there? Lithos must think well of you.
- **The Zealans** (after `ancient_ones`)
  > My grandfather spoke of the Zealans. Folk who prayed to no Titan at all, he said, and built in white stone.
  > They are gone, long before his time. If you have been walking in their ruins, I hope you left them as you found them.
  - *Avatar:* "What became of them?"
    > The old songs say they warred with our people and lost. The songs do not say why. Songs seldom do.
- **Hydros** (after `free_hydros`)
  > Have you seen the streams? They came down the valley white and loud three nights ago, and the well is full to the brim.
  > Corinth says the Lady of the Lake is awake again. Funny, Mordea made us honour her, and now she has outlived Mordea.
  - *Avatar:* "I set her free."
    > You did? Then mind yourself. A Titan freed is a Titan who remembers who held the chains.
  - *Avatar:* "Is it good for the crops?"
    > Too much of a good thing drowns the roots. But the grass for the torax has never looked so green.
- **The wind** (after `test_belief`)
  > The wind has changed. It comes off the sea steady now, like a thing that has made up its mind.
  > Cyrrus always said the Theurgists could feel the moods of Stratos. I think even an old herdsman can feel this one.
  - *Avatar:* "I stood before Stratos."
    > Of course you did. Air, earth, what next? I will stop being surprised by you when you stop surprising me.
- **The fire isle** (after `pyros_ceremony`)
  > There was a red glow over the sea toward Morgaelin. The whole sky looked like a forge with the door left open.
  > The sorcerers live out there. Whatever they did, I hope they keep it on their own island.
  - *instead, after `kill_malchir`:*
    > The glow over Morgaelin is gone. Folk say the master of the sorcerers is dead, and that you had a hand in it.
    > I do not want to know the details. I only want my son to stay well away from fire.
  - *Avatar:* "Are you afraid?"
    > Of fire? Every herdsman is. One dry summer and one spark, and a lifetime of work is ash.
- **Blackrock** (after `show_tongue`)
  > You carry yourself like a man about to set off on a long road. Corinth noticed it first.
  > Mythran was by, asking after a black stone. I told him we have only grey ones. Is that your business too?
  - *Avatar:* "I may be leaving."
    > Leaving Pagan? There is nowhere else. Or so I always believed.
    > Well. If you find a road off this island, you take it. Only stop by first and let Corinth feed you.
  - *Avatar:* "What do you know of it?"
    > Nothing, friend. Black stone is a sorcerer's worry. My worries have four legs and horns.
- **The Titans** (after `ethereal_travel`)
  > The sky has been wrong these last days. Too still. The torax will not graze and the hens will not lay.
  > My whole life I said the Titans were the only magic in the world. I think you are about to prove me wrong.
  - *Avatar:* "What if they fall?"
    > Then the rain will still come, I think, and the grass will still grow. It did before anyone named them.
    > And a man will still have to get up before dawn to tend his flock. That is a comfort, in its way.

### Slot: son

- **Cyrrus** (after `to_argentrock`; until `ask_cyrrus`)
  > You have been out to Argentrock? Then you might have seen my boy. Cyrrus. Tall, freckled, never stops washing his hands.
  > If you see him, tell him his mother asks whether he eats. That is all she ever asks.
  - *Avatar:* "I have not met him."
    > He will be at the healers' house, I should think. The Theurgists keep him busy. Look for him there.
- **Your son** (after `ask_cyrrus`)
  > Corinth has had a letter. She says you met our Cyrrus on the isle and that he spoke of you. Is he well?
  - *instead, after `test_skills`:*
    > Cyrrus writes that you healed a torax under the Theurgists' eyes. A torax!
    > Then you have done what I do every spring, with none of my swearing. Perhaps you missed your calling.
  - *Avatar:* "He is well."
    > Good. Good. Do not tell Corinth I asked twice. She thinks I do not worry.
  - *Avatar:* "Will he come home?"
    > To visit, I hope. To stay, no. He has his calling, and I have mine. That is how it should be.
