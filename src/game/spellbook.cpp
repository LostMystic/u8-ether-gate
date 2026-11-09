// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
// The spellbook (game/spellbook.h, NOTES.md "Spellbook"). Spell facts: specs/world/magic.md 3-5 and
// specs/world/leveling_facts.md 1.1-1.4 (costs, entries, recipes); the casting rules are the engine's.
#include "game/spellbook.h"

#include <algorithm>

#include "core/log.h"
#include "core/serial.h"
#include "game/processes.h"
#include "game/world.h"
#include "kernel/kernel.h"
#include "usecode/uc_machine.h"

namespace pg {

namespace {
constexpr unsigned kToken = 746, kAirFocus = 396, kFireFocus = 397, kGrimoire = 288, kScroll = 827;
constexpr unsigned kPortalScroll = 1;  // 827 quality 1: lasting, a story item (critical_path.md 7)
constexpr unsigned kEarthReagent = 395, kFireReagent = 398;
constexpr unsigned kKeyShape = 232, kPentacleShape = 124;
constexpr unsigned kChargedBook = 11;
constexpr unsigned kTempFocusFrame = 12;  // a charged symbol: takes every sorcery spell (magic.md 3.1)
// Necromancy reagent kinds (shape 395; bugfixes.md 2.2 frame groups).
enum { kBlood, kBone, kWood, kDirt, kHood, kBlackmoor };
// Sorcery reagent kinds (shape 398; magic.md 3.1).
enum { kAsh, kPumice, kObsidian, kIron, kBrimstone, kDaemonBone };

bool pid_live(World& w, uint16_t pid) {
    if (!pid) return false;
    Process* p = w.kernel().get(pid);
    return p && p->live();
}
}  // namespace

const char* Spellbook::school_name(int s) {
    static const char* n[] = {"Necromancy", "Theurgy", "Sorcery", "Thaumaturgy", "Tempest", "Scrolls"};
    return s >= 0 && s < kSchools ? n[s] : "";
}

const std::vector<Spellbook::Spell>& Spellbook::spells() {
    static const std::vector<Spell> v = {
        // Necromancy: token 746, frame = spell; cost from ERTHSPEL (leveling_facts.md 1.1), reagents magic.md 4.3.
        {kNecromancy, 0, "Death Speak", "Kal Wis Corp", 1, 1, {{kBlood, 1}, {kBone, 1}}, kToken, 0},
        {kNecromancy, 1, "Mask of Death", "Quas Corp", 1, 1, {{kWood, 1}, {kHood, 1}}, kToken, 1},
        {kNecromancy, 2, "Rock Flesh", "Rel Sanct Ylem", 2, 2, {{kWood, 1}, {kDirt, 1}}, kToken, 2},
        {kNecromancy, 3, "Summon Dead", "Kal Corp Xen", 2, 2, {{kBlood, 1}, {kBone, 1}, {kWood, 1}}, kToken, 3},
        {kNecromancy, 4, "Open Ground", "Des Por Ylem", 3, 3, {{kBlood, 1}, {kBlackmoor, 1}}, kToken, 4},
        {kNecromancy, 5, "Create Golem", "In Ort Ylem Xen", 3, 3,
         {{kBlood, 1}, {kBone, 1}, {kWood, 1}, {kDirt, 1}, {kBlackmoor, 1}}, kToken, 5},
        {kNecromancy, 6, "Withstand Death", "Vas An Corp", 4, 4, {{kWood, 1}, {kDirt, 1}, {kBlackmoor, 1}}, kToken, 6},
        {kNecromancy, 7, "Grant Peace", "In Vas Corp", 5, 5, {{kHood, 1}, {kBlackmoor, 1}}, kToken, 7},
        {kNecromancy, 8, "Call Quake", "Vas Por Ylem", 5, 5, {{kBone, 1}, {kWood, 1}, {kDirt, 1}, {kBlackmoor, 1}}, kToken, 8},
        // Theurgy: focus 396, frame = spell; cost from AIRSPEL (leveling_facts.md 1.2). No reagents.
        {kTheurgy, 8, "Divination", "In Wis", 3, 3, {}, kAirFocus, 8},
        {kTheurgy, 9, "Healing Touch", "In Mani", 5, 5, {}, kAirFocus, 9},
        {kTheurgy, 10, "Aerial Servant", "Kal Ort Xen", 5, 5, {}, kAirFocus, 10},
        {kTheurgy, 11, "Restoration", "Vas In Mani", 15, 15, {}, kAirFocus, 11},
        {kTheurgy, 12, "Fade From Sight", "Quas An Lor", 5, 5, {}, kAirFocus, 12},
        {kTheurgy, 13, "Air Walk", "", 15, 15, {}, kAirFocus, 13},
        {kTheurgy, 14, "Hear Truth", "An Quas Lor", 3, 3, {}, kAirFocus, 14},
        {kTheurgy, 15, "Intervention", "", 15, 15, {}, kAirFocus, 15},
        {kTheurgy, 17, "Reveal", "Ort Lor", 5, 5, {}, kAirFocus, 17},
        // Sorcery: focus 397 quality high byte = spell; the enchanting cost range and reagents (magic.md 3.2, 3.3).
        {kSorcery, 1, "Ignite", "In Flam", 3, 2, {{kAsh, 1}, {kPumice, 2}}, kFireFocus, 12},
        {kSorcery, 2, "Extinguish", "An Flam", 3, 2, {{kPumice, 3}}, kFireFocus, 12},
        {kSorcery, 3, "Flash", "Flam Por", 5, 3, {{kAsh, 2}, {kPumice, 1}}, kFireFocus, 12},
        {kSorcery, 4, "Flame Bolt", "In Ort Flam", 6, 4, {{kAsh, 1}, {kPumice, 1}, {kIron, 1}}, kFireFocus, 12},
        {kSorcery, 5, "Endure Heat", "Sanct Flam", 5, 3, {{kObsidian, 2}, {kIron, 1}}, kFireFocus, 12},
        {kSorcery, 6, "Fire Shield", "In Flam An Por", 6, 4, {{kAsh, 1}, {kObsidian, 2}, {kIron, 1}}, kFireFocus, 12},
        {kSorcery, 7, "Armor of Flames", "Vas Sanct Flam", 8, 5,
         {{kAsh, 1}, {kObsidian, 2}, {kIron, 1}, {kBrimstone, 1}}, kFireFocus, 12},
        {kSorcery, 8, "Create Fire", "In Flam Ylem", 9, 6, {{kAsh, 2}, {kPumice, 1}, {kObsidian, 1}}, kFireFocus, 12},
        {kSorcery, 9, "Explosion", "Vas Ort Flam", 10, 7, {{kAsh, 1}, {kPumice, 1}, {kIron, 1}, {kBrimstone, 1}}, kFireFocus, 12},
        {kSorcery, 10, "Summon Daemon", "Kal Flam Corp Xen", 13, 8,
         {{kAsh, 1}, {kPumice, 1}, {kObsidian, 2}, {kDaemonBone, 1}}, kFireFocus, 12},
        {kSorcery, 11, "Banish Daemon", "An Flam Corp Xen", 13, 8,
         {{kAsh, 1}, {kPumice, 1}, {kIron, 2}, {kDaemonBone, 1}}, kFireFocus, 12},
        {kSorcery, 12, "Conflagration", "Kal Vas Flam Corp Xen", 15, 10,
         {{kAsh, 1}, {kPumice, 1}, {kObsidian, 1}, {kIron, 1}, {kBrimstone, 1}, {kDaemonBone, 3}}, kFireFocus, 12},
        // Thaumaturgy: book 288 by quality, cast when charged (frame 11), no mana (leveling_facts.md 1.4).
        // The original's charging (ENCHANT) never finishes in v2.10 (QUESTIONS.md), so no reagents count.
        {kThaumaturgy, 81, "Confusion Blast", "", 0, 0, {}, kGrimoire, kChargedBook},
        {kThaumaturgy, 82, "Meteor Shower", "Kal Des Flam Ylem", 0, 0, {}, kGrimoire, kChargedBook},
        {kThaumaturgy, 83, "Summon Creature", "", 0, 0, {}, kGrimoire, kChargedBook},
        {kThaumaturgy, 84, "Call Destruction", "", 0, 0, {}, kGrimoire, kChargedBook},
        {kThaumaturgy, 85, "Devastation", "", 0, 0, {}, kGrimoire, kChargedBook},
        {kThaumaturgy, 86, "Ethereal Travel", "", 0, 0, {}, kGrimoire, kChargedBook},
        // Tempest (water): none yet.
        // Scrolls: shape 827 by quality (leveling_facts.md 7, critical_path.md 7; class 1401 SCROLL2).
        {kScrolls, 1, "Dispel Magic Portals", "", 0, 0, {}, kScroll, 0},
        {kScrolls, 50, "Detect Trapped Chests", "", 0, 0, {}, kScroll, 0},
        {kScrolls, 51, "Destroy Chest Traps", "", 0, 0, {}, kScroll, 0},
    };
    return v;
}

const Spellbook::Spell* Spellbook::find(int key) {
    for (const Spell& s : spells())
        if (s.key() == key) return &s;
    return nullptr;
}

int Spellbook::reagent_kind(School school, unsigned shape, unsigned f) {
    if (school == kNecromancy && shape == kEarthReagent) {
        if (f <= 5) return kBlood;
        if (f <= 7) return kBone;
        if (f == 8) return kWood;
        if (f == 9) return kDirt;
        if (f <= 12) return kHood;
        if (f == 14 || f == 15) return kBlackmoor;
        return -1;
    }
    if (school == kSorcery && shape == kFireReagent) {
        if (f <= 1) return kAsh;
        if (f <= 5) return kPumice;
        if (f <= 9) return kObsidian;
        if (f <= 13) return kIron;
        if (f <= 17) return kBrimstone;
        if (f <= 20) return kDaemonBone;
    }
    return -1;
}

// ---------------------------------------------------------------------------------------------
// Watches: what follows a use event (learning a spell, or the book's own clean-up after a cast).

struct Spellbook::Watch {
    enum Kind { kManaLearn, kFocusLearn, kBookLearn, kMakeTokens, kPentacle, kAfterCast } kind;
    uint16_t pid = 0;
    uint32_t start = 0;
    int key = 0;
    int mana0 = 0;
    uint16_t item = 0, q0 = 0, f0 = 0;
    std::vector<std::pair<uint16_t, uint16_t>> foci;  // pentagram: focus id, quality before
    // kAfterCast
    uint16_t restore_parent = 0, temp_item = 0;
    bool restore_stats = false;
    int mana_before = 0, intel_before = 0, acc_before = 0;
    std::vector<std::pair<unsigned, uint16_t>> globals;  // G[pos:1] to put back
    bool done = false;
};

Spellbook::Spellbook(World& w) : w_(w) {}
Spellbook::~Spellbook() = default;

void Spellbook::reset() {
    known.clear();
    watches_.clear();
    log.clear();
    internal_use = false;
}

void Spellbook::learn(int key) {
    if (!find(key) || known.count(key)) return;
    known.insert(key);
    const Spell* s = find(key);
    log.push_back(std::string("learned ") + s->name);
    PG_LOG("spellbook: %s entered in the book", s->name);
}

void Spellbook::inventory(std::vector<uint16_t>& out) const {
    out.clear();
    std::vector<uint16_t> stack = {1};
    while (!stack.empty()) {
        uint16_t id = stack.back();
        stack.pop_back();
        const Item* it = w_.item(id);
        if (!it) continue;
        for (uint16_t c : it->contents) {
            out.push_back(c);
            if (const Item* ci = w_.item(c); ci && !ci->contents.empty()) stack.push_back(c);
        }
    }
}

std::vector<const Spellbook::Spell*> Spellbook::listed() const {
    std::vector<const Spell*> v;
    std::vector<uint16_t> inv;
    bool scanned = false;
    for (const Spell& s : spells()) {
        if (s.school == kScrolls && !debug_infinite) {  // while one is carried
            if (!scanned) { inventory(inv); scanned = true; }
            if (find_talisman(s, inv)) v.push_back(&s);
            continue;
        }
        if (debug_infinite || known.count(s.key())) v.push_back(&s);
    }
    return v;
}

namespace {
int quantity(const Item& it) { return it.quality ? it.quality : 1; }
}

int Spellbook::count_reagent_sets(const Spell& s, const std::vector<uint16_t>& inv) const {
    if (s.reagents.empty()) return 0;
    int have[8] = {};
    for (uint16_t id : inv)
        if (const Item* it = w_.item(id)) {
            int k = reagent_kind(s.school, it->shape, it->frame);
            if (k >= 0 && k < 8) have[k] += quantity(*it);
        }
    int sets = 1 << 20;
    for (const Reagent& r : s.reagents) sets = std::min(sets, have[r.kind] / std::max(1, r.count));
    return sets;
}

void Spellbook::use_up_reagents(const Spell& s) {
    std::vector<uint16_t> inv;
    inventory(inv);
    for (const Reagent& r : s.reagents) {
        int need = r.count;
        for (uint16_t id : inv) {
            if (need <= 0) break;
            Item* it = w_.item(id);
            if (!it || !it->alive || reagent_kind(s.school, it->shape, it->frame) != r.kind) continue;
            while (need > 0 && it->quality > 1) { --it->quality; --need; }
            if (need > 0) { --need; w_.destroy(id, false); }
        }
    }
}

uint16_t Spellbook::find_talisman(const Spell& s, const std::vector<uint16_t>& inv) const {
    uint16_t best = 0;
    for (uint16_t id : inv) {
        const Item* it = w_.item(id);
        if (!it || !it->alive) continue;
        switch (s.school) {
        case kNecromancy: if (it->shape == kToken && it->frame == unsigned(s.num)) return id; break;
        case kTheurgy: if (it->shape == kAirFocus && it->frame == unsigned(s.num)) return id; break;
        case kSorcery:
            // A charged focus holding this spell; the four-symbols item (frame 20) last, as it can explode.
            if (it->shape == kFireFocus && (it->quality >> 8) == unsigned(s.num) && (it->quality & 0xFF) > 0) {
                if (it->frame != 20) return id;
                if (!best) best = id;
            }
            break;
        case kThaumaturgy:
            if (it->shape == kGrimoire && it->quality == unsigned(s.num) && it->frame == kChargedBook) return id;
            break;
        case kScrolls: if (it->shape == kScroll && it->quality == unsigned(s.num)) return id; break;
        default: break;
        }
    }
    return best;
}

Spellbook::Status Spellbook::status(const Spell& s) const {
    Status st;
    const Actor* av = w_.avatar();
    const int mana = av ? av->mana : 0;
    if (debug_infinite) {
        st.casts = -1;
        st.castable = true;
        st.note = "Debug: unlimited";
        return st;
    }
    std::vector<uint16_t> inv;
    inventory(inv);
    for (uint16_t id : inv) {
        const Item* it = w_.item(id);
        if (!it) continue;
        switch (s.school) {
        case kNecromancy: if (it->shape == kToken && it->frame == unsigned(s.num)) ++st.talismans; break;
        case kTheurgy: if (it->shape == kAirFocus && it->frame == unsigned(s.num)) st.focus = true; break;
        case kSorcery:
            if (it->shape == kFireFocus && (it->quality >> 8) == unsigned(s.num)) st.talismans += it->quality & 0xFF;
            break;
        case kThaumaturgy:
            if (it->shape == kGrimoire && it->quality == unsigned(s.num) && it->frame == kChargedBook) ++st.talismans;
            break;
        case kScrolls: if (it->shape == kScroll && it->quality == unsigned(s.num)) ++st.talismans; break;
        default: break;
        }
    }
    st.reagent_casts = count_reagent_sets(s, inv);
    switch (s.school) {
    case kNecromancy:
        st.casts = st.talismans + st.reagent_casts;
        st.mana_casts = mana / std::max(1, s.cost);
        break;
    case kTheurgy:
        st.casts = st.focus ? -1 : 0;
        st.mana_casts = mana / std::max(1, s.cost);
        if (!st.focus) st.note = "No focus";
        // After the Test of Belief only Air Walk still answers (leveling_facts.md 1.2).
        else if (s.num != 13 && w_.vm().get_global(56, 1)) { st.note = "Stratos is silent"; st.casts = 0; }
        break;
    case kSorcery: {
        // Charges cost no mana; each reagent cast pays the enchanting cost (its top, to be sure).
        const int by_mana = mana / std::max(1, s.cost);
        st.casts = st.talismans + st.reagent_casts;
        st.mana_casts = st.talismans + std::min(st.reagent_casts, by_mana);
        if (st.reagent_casts == 0) st.mana_casts = -1;
        break;
    }
    case kThaumaturgy: case kScrolls: st.casts = st.talismans; break;
    default: break;
    }
    const int usable = st.casts < 0 ? (st.mana_casts < 0 ? 1 : st.mana_casts)
                                     : (st.mana_casts < 0 ? st.casts : std::min(st.casts, st.mana_casts));
    st.castable = usable > 0;
    if (st.note.empty() && !st.castable) {
        if (st.casts == 0) st.note = s.school == kNecromancy || s.school == kSorcery ? "No reagents" : "None charged";
        else st.note = "Need " + std::to_string(s.cost) + " mana";
    }
    return st;
}

uint16_t Spellbook::fire(uint16_t item) {
    internal_use = true;
    uint16_t pid = w_.call_event(item, kEvUse);
    internal_use = false;
    return pid;
}

bool Spellbook::cast(const Spell& s, std::string& why) {
    Actor* av = w_.avatar();
    if (!av || av->is(Actor::kDead)) { why = "You cannot cast now"; return false; }
    if (w_.stasis || w_.target_pid) { why = "You cannot cast now"; return false; }
    if (s.school == kTempest) { why = "Not yet"; return false; }
    if (debug_infinite) return debug_cast(s, why);
    Status st = status(s);
    if (!st.castable) { why = st.note.empty() ? "You cannot cast that" : st.note; return false; }
    std::vector<uint16_t> inv;
    inventory(inv);
    uint16_t tal = find_talisman(s, inv);
    auto watch = std::make_unique<Watch>();
    watch->kind = Watch::kAfterCast;
    watch->start = w_.kernel().frame_number();
    switch (s.school) {
    case kNecromancy: {
        if (av->mana < s.cost) { why = "Need " + std::to_string(s.cost) + " mana"; return false; }
        if (!tal) {  // the reagents into a token, as the key would make it
            use_up_reagents(s);
            tal = w_.create_item(kToken, unsigned(s.num));
            if (!w_.give_to_avatar(tal) && !w_.move_to_container(tal, 1)) { w_.destroy_now(tal); why = "No room"; return false; }
        }
        watch->pid = fire(tal);
        break;
    }
    case kTheurgy:
        if (!tal) { why = "No focus"; return false; }
        watch->pid = fire(tal);
        break;
    case kSorcery:
        if (!tal) {
            if (av->mana < s.cost) { why = "Need " + std::to_string(s.cost) + " mana"; return false; }
            use_up_reagents(s);
            av->mana = int16_t(av->mana - (s.cost_min + w_.rand_below(s.cost - s.cost_min + 1)));
            tal = w_.create_item(kFireFocus, kTempFocusFrame);
            w_.item(tal)->quality = uint16_t((s.num << 8) | 1);
            if (!w_.move_to_container(tal, 1)) { w_.destroy_now(tal); why = "No room"; return false; }
            watch->temp_item = tal;
        } else if (const Item* it = w_.item(tal); it && it->parent != 1) {
            // A focus must be held (in the Avatar's own hands, not the backpack) to invoke it.
            watch->restore_parent = it->parent;
            watch->item = tal;
            w_.move_to_container(tal, 1);
        }
        watch->pid = fire(tal);
        break;
    case kThaumaturgy:
        if (!tal) { why = "None charged"; return false; }
        watch->pid = fire(tal);
        break;
    case kScrolls:
        if (!tal) { why = "No scroll"; return false; }
        watch->pid = fire(tal);
        if (s.num != int(kPortalScroll)) watch->temp_item = tal;  // used up once read from the book
        break;
    default: return false;
    }
    last_cast_pid = watch->pid;
    log.push_back(std::string("cast ") + s.name);
    watches_.push_back(std::move(watch));
    return true;
}

bool Spellbook::debug_cast(const Spell& s, std::string& why) {
    Actor* av = w_.avatar();
    auto watch = std::make_unique<Watch>();
    watch->kind = Watch::kAfterCast;
    watch->start = w_.kernel().frame_number();
    // Nothing the Avatar owns is used, and the cast leaves his mana, INT and the story as they were.
    watch->restore_stats = true;
    watch->mana_before = av->mana;
    watch->intel_before = av->intel;
    watch->acc_before = w_.stat_accumulator[1];
    av->mana = int16_t(std::max<int>(av->mana, 99));
    uint16_t tmp = 0;
    switch (s.school) {
    case kNecromancy: tmp = w_.create_item(kToken, unsigned(s.num)); break;
    case kTheurgy:
        tmp = w_.create_item(kAirFocus, unsigned(s.num));
        if (w_.vm().get_global(56, 1)) { watch->globals.push_back({56, 1}); w_.vm().set_global(56, 1, 0); }
        break;
    case kSorcery:
        tmp = w_.create_item(kFireFocus, kTempFocusFrame);
        w_.item(tmp)->quality = uint16_t((s.num << 8) | 1);
        if (!w_.vm().get_global(371, 1) && !w_.vm().get_global(385, 1)) {  // a focus needs a mentor's deal
            watch->globals.push_back({371, 0});
            w_.vm().set_global(371, 1, 1);
        }
        break;
    case kThaumaturgy:
        tmp = w_.create_item(kGrimoire, kChargedBook);
        w_.item(tmp)->quality = uint16_t(s.num);
        break;
    case kScrolls:
        tmp = w_.create_item(kScroll, 0);
        w_.item(tmp)->quality = uint16_t(s.num);
        break;
    default: why = "Not yet"; return false;
    }
    if (!w_.move_to_container(tmp, 1)) { w_.destroy_now(tmp); why = "No room"; return false; }
    watch->temp_item = tmp;
    watch->pid = fire(tmp);
    last_cast_pid = watch->pid;
    log.push_back(std::string("debug cast ") + s.name);
    watches_.push_back(std::move(watch));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Hooks

void Spellbook::on_use(uint16_t id, uint16_t pid) {
    if (internal_use) return;
    const Item* it = w_.item(id);
    const Actor* av = w_.avatar();
    if (!it || !av) return;
    auto add = [&](Watch::Kind k, int key) -> Watch& {
        auto wt = std::make_unique<Watch>();
        wt->kind = k;
        wt->pid = pid;
        wt->start = w_.kernel().frame_number();
        wt->key = key;
        wt->mana0 = av->mana;
        wt->item = id;
        wt->q0 = it->quality;
        wt->f0 = it->frame;
        watches_.push_back(std::move(wt));
        return *watches_.back();
    };
    // Spells cast the normal way enter the book once they really went off (mana spent, a charge used).
    if (it->shape == kToken && it->frame <= 8) add(Watch::kManaLearn, kNecromancy * 1000 + it->frame);
    else if (it->shape == kAirFocus && find(kTheurgy * 1000 + it->frame)) add(Watch::kManaLearn, kTheurgy * 1000 + it->frame);
    else if (it->shape == kFireFocus && find(kSorcery * 1000 + (it->quality >> 8)) && (it->quality & 0xFF))
        add(Watch::kFocusLearn, kSorcery * 1000 + (it->quality >> 8));
    else if (it->shape == kGrimoire && find(kThaumaturgy * 1000 + it->quality)) {
        if (it->frame == kChargedBook) learn(kThaumaturgy * 1000 + it->quality);  // casts, no mana, never fails
        else add(Watch::kBookLearn, kThaumaturgy * 1000 + it->quality);           // charging it
    }
    // Making: the key on a bag of reagents, the pentagram charging a focus.
    else if (it->shape == kKeyShape && it->frame != 3) add(Watch::kMakeTokens, 0);
    else if (it->shape == kPentacleShape) {
        Watch& wt = add(Watch::kPentacle, 0);
        std::vector<uint16_t> near;
        w_.area_search(it->x, it->y, 400, false, near);
        for (uint16_t f : near)
            if (const Item* fi = w_.item(f); fi && fi->shape == kFireFocus) wt.foci.push_back({f, fi->quality});
    }
}

void Spellbook::on_create(uint16_t id) {
    const Item* it = w_.item(id);
    if (!it || it->shape != kToken || it->frame > 8 || internal_use) return;
    for (auto& wt : watches_)
        if (wt->kind == Watch::kMakeTokens && !wt->done) { learn(kNecromancy * 1000 + it->frame); return; }
}

void Spellbook::frame() {
    if (watches_.empty()) return;
    Actor* av = w_.avatar();
    const uint32_t now = w_.kernel().frame_number();
    for (auto& wp : watches_) {
        Watch& wt = *wp;
        const uint32_t age = now - wt.start;
        const bool live = pid_live(w_, wt.pid);
        switch (wt.kind) {
        case Watch::kManaLearn:
            if (av && av->mana < wt.mana0) { learn(wt.key); wt.done = true; }
            else if (av) wt.mana0 = std::max<int>(wt.mana0, av->mana);
            if (age > 1800) wt.done = true;
            break;
        case Watch::kFocusLearn: {
            const Item* it = w_.item(wt.item);
            if (!it || !it->alive || (it->quality & 0xFF) < (wt.q0 & 0xFF) || it->quality != wt.q0) { learn(wt.key); wt.done = true; }
            if (age > 1800) wt.done = true;
            break;
        }
        case Watch::kBookLearn: {
            const Item* it = w_.item(wt.item);
            if (it && it->frame == kChargedBook) { learn(wt.key); wt.done = true; }
            if (age > 1800 || !it) wt.done = true;
            break;
        }
        case Watch::kMakeTokens:
            if ((!live && age > 60 && !w_.target_pid) || age > 3600) wt.done = true;
            break;
        case Watch::kPentacle:
            for (auto& f : wt.foci) {
                const Item* it = w_.item(f.first);
                if (!it || it->quality == f.second) continue;
                const int spell = it->quality >> 8;
                if ((it->quality & 0xFF) > 0 && find(kSorcery * 1000 + spell)) learn(kSorcery * 1000 + spell);
                f.second = it->quality;
            }
            if ((!live && age > 30) || age > 3600) wt.done = true;
            break;
        case Watch::kAfterCast:
            if (!((age >= 90 && !live && !w_.target_pid) || age > 3600)) break;
            if (wt.item && wt.restore_parent) {
                Item* it = w_.item(wt.item);
                if (it && it->alive && it->parent == 1) {
                    const Item* back = w_.item(wt.restore_parent);
                    if (!back || !back->alive || !w_.move_to_container(wt.item, wt.restore_parent)) w_.give_to_avatar(wt.item);
                }
            }
            if (wt.temp_item) if (Item* it = w_.item(wt.temp_item); it && it->alive) w_.destroy(wt.temp_item, false);
            for (auto& g : wt.globals) w_.vm().set_global(g.first, 1, g.second);
            if (wt.restore_stats && av) {
                av->mana = int16_t(wt.mana_before);
                av->intel = int16_t(wt.intel_before);
                w_.stat_accumulator[1] = wt.acc_before;
            }
            wt.done = true;
            break;
        }
    }
    watches_.erase(std::remove_if(watches_.begin(), watches_.end(), [](const std::unique_ptr<Watch>& p) { return p->done; }),
                   watches_.end());
}

// ---------------------------------------------------------------------------------------------
// Saves

void Spellbook::save(SaveWriter& o) const {
    o.tag("SPBK");
    o.u8(1);
    o.u32(uint32_t(known.size()));
    for (int k : known) o.i32(k);
}

bool Spellbook::load(SaveReader& in) {
    reset();
    if (!in.tag("SPBK")) return false;
    if (in.u8() != 1) return false;
    uint32_t n = std::min<uint32_t>(in.u32(), 10000);
    for (uint32_t i = 0; i < n && !in.fail(); ++i) {
        int k = in.i32();
        if (find(k)) known.insert(k);
    }
    return !in.fail();
}

}  // namespace pg
