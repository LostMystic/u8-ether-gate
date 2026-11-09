// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// Milestone 2 tests: the kernel, VM opcodes on hand-built bytecode, loading every usecode
// class of EUSECODE.FLX, and a headless run of the new-game startup.
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "check.h"
#include "data/gamedata.h"
#include "game/world.h"
#include "kernel/kernel.h"
#include "usecode/uc_machine.h"
#include "usecode/usecode_file.h"

using namespace pg;

// ---------------------------------------------------------------------------------------------
// Kernel

namespace {
struct CountProc : Process {
    std::vector<int>* log;
    int tag, runs = 0, stop_after;
    CountProc(std::vector<int>* l, int t, int stop) : log(l), tag(t), stop_after(stop) {}
    void run() override {
        log->push_back(tag);
        if (++runs >= stop_after) terminate();
    }
};
}  // namespace

void test_kernel() {
    Kernel k;
    std::vector<int> log;
    uint16_t a = k.add(std::make_unique<CountProc>(&log, 1, 3));
    uint16_t b = k.add(std::make_unique<CountProc>(&log, 2, 1));
    CHECK_EQ(a, 1);
    CHECK_EQ(b, 2);
    k.pass();  // tick 1: odd, nothing runs
    CHECK(log.empty());
    k.pass();  // tick 2: b was added last at the front, so it runs first
    CHECK_EQ(log.size(), 2u);
    CHECK_EQ(log[0], 2);
    CHECK_EQ(log[1], 1);
    CHECK(k.get(b) == nullptr);  // terminated and removed in the same pass
    CHECK_EQ(k.tick(), 2u);
    CHECK_EQ(k.frame_number(), 1u);
    // Waiting: c waits for a; a ends on its 3rd run and c wakes with a's result.
    auto* c = new CountProc(&log, 3, 100);
    uint16_t cp = k.add(std::unique_ptr<Process>(c));
    c->wait_for(a);
    CHECK(c->suspended());
    k.get(a)->result = 77;
    log.clear();
    k.frame();
    k.frame();
    CHECK(k.get(a) == nullptr);
    CHECK(!c->suspended());
    CHECK_EQ(c->result, 77u);
    // The woken waiter is moved after the process that ended, so it ran in the same pass.
    CHECK_EQ(log.back(), 3);
    // Kills: by (item, type), item 0 never matched.
    c->item = 5;
    c->type = 0x205;
    CHECK_EQ(k.count(5, kProcAny), 1u);
    CHECK_EQ(k.kill(0, 0x205), 1u);
    CHECK(c->failed());
    k.frame();
    CHECK(k.get(cp) == nullptr);
    // Deferred termination: still present for a pass, but no longer live.
    auto* d = new CountProc(&log, 4, 100);
    uint16_t dp = k.add(std::unique_ptr<Process>(d));
    d->terminate_deferred();
    CHECK(!d->live());
    CHECK(k.get(dp) != nullptr);
    k.pass();
    CHECK(k.get(dp) == nullptr);
    // Pause: the tick does not advance and nothing runs.
    uint32_t t = k.tick();
    k.pause();
    k.frame();
    CHECK_EQ(k.tick(), t);
    k.unpause();
    // Id allocator: lowest first, freed ids go to the back.
    IdAllocator ids(1, 4);
    CHECK_EQ(ids.allocate(), 1);
    CHECK_EQ(ids.allocate(), 2);
    ids.release(1);
    CHECK_EQ(ids.allocate(), 3);
    CHECK(ids.reserve(4));
    CHECK_EQ(ids.allocate(), 1);
    CHECK_EQ(ids.allocate(), 0);
}

// ---------------------------------------------------------------------------------------------
// VM opcodes on hand-built bytecode

namespace {

// A tiny assembler for test code.
struct Asm {
    std::vector<uint8_t> b;
    Asm& op(uint8_t o) { b.push_back(o); return *this; }
    Asm& u8(int v) { b.push_back(uint8_t(v)); return *this; }
    Asm& u16(int v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); return *this; }
    Asm& u32(uint32_t v) { u16(int(v & 0xFFFF)); return u16(int(v >> 16)); }
    Asm& pushb(int v) { return op(0x0A).u8(v); }
    Asm& pushw(int v) { return op(0x0B).u16(v); }
    Asm& str(const std::string& s) { op(0x0D).u16(int(s.size())); for (char c : s) b.push_back(uint8_t(c)); return u8(0); }
    Asm& rec() { return op(0x0F).u8(2).u16(0x100).op(0x6E).u8(0xFE); }  // record(top word); addsp -2
    Asm& recstr() { return op(0x6B).op(0x0F).u8(4).u16(0x101).op(0x65).u8(0).op(0x6E).u8(0xFC); }
    unsigned here() const { return 0x80 + unsigned(b.size()); }
};

// Builds a synthetic EUSECODE.FLX with the given classes (code starting at address 0x80,
// event table entries `events`).
Bytes build_flex(const std::map<unsigned, std::pair<std::vector<uint8_t>, std::map<unsigned, unsigned>>>& classes) {
    const unsigned n = UsecodeFile::kClasses + 2;
    std::vector<Bytes> entries(n);
    entries[1].assign(UsecodeFile::kClasses * 13, 0);
    for (auto& [c, cls] : classes) {
        Bytes o(0x0C + 0x80, 0);
        o.insert(o.end(), cls.first.begin(), cls.first.end());
        uint32_t size = uint32_t(o.size());
        std::memcpy(&o[4], &size, 4);
        for (auto& [e, addr] : cls.second) std::memcpy(&o[0x0C + 4 * e], &addr, 4);
        entries[c + 2] = o;
        uint32_t mask = 0;
        for (auto& [e, addr] : cls.second) mask |= 1u << e;
        std::memcpy(&entries[1][13 * c], &mask, 4);
        std::memcpy(&entries[1][13 * c + 4], "TEST", 4);
    }
    Bytes f(0x80 + 8 * n, 0);
    for (unsigned i = 0; i < 0x52; ++i) f[i] = 0x1A;  // Flex title padding
    std::memcpy(&f[0x54], &n, 4);
    for (unsigned i = 0; i < n; ++i) {
        uint32_t off = uint32_t(f.size()), sz = uint32_t(entries[i].size());
        std::memcpy(&f[0x80 + 8 * i], &off, 4);
        std::memcpy(&f[0x80 + 8 * i + 4], &sz, 4);
        f.insert(f.end(), entries[i].begin(), entries[i].end());
    }
    return f;
}

struct FakeHost : UsecodeHost {
    std::set<uint16_t> alive{5, 6, 7};
    std::vector<int> types;
    void loop_search(int type, uint16_t a, uint16_t b, const uint8_t* script, unsigned len,
                     std::vector<uint16_t>& out) override {
        types.push_back(type);
        (void)a; (void)b; (void)script; (void)len;
        out = {5, 6, 7};
    }
    bool object_exists(uint16_t id) const override { return alive.count(id) != 0; }
};

struct VmRig {
    UsecodeFile file;
    Kernel kernel;
    std::unique_ptr<UCMachine> vm;
    FakeHost host;
    std::vector<int> rec;
    std::vector<std::string> recs;
    bool ok = false;
    explicit VmRig(const std::map<unsigned, std::pair<std::vector<uint8_t>, std::map<unsigned, unsigned>>>& classes) {
        ok = file.load(build_flex(classes));
        vm = std::make_unique<UCMachine>(file, kernel);
        vm->set_host(&host);
        vm->set_intrinsic(0x100, [this](IntrinsicCall& c) -> uint32_t { rec.push_back(c.s16(0)); return 0; });
        vm->set_intrinsic(0x101, [this](IntrinsicCall& c) -> uint32_t { recs.push_back(c.str(0)); return 0; });
        vm->set_intrinsic(0x0FA, [](IntrinsicCall& c) -> uint32_t { return uint32_t(c.u16(0)) * 2; });
    }
    void run(int frames) { for (int i = 0; i < frames; ++i) kernel.frame(); }
};

}  // namespace

void test_vm() {
    // --- arithmetic, compares, shifts, logic
    Asm a;
    a.op(0x5A).u8(7);                                       // enter 7 -> 8 bytes of locals
    a.pushb(7).pushb(3).op(0x1C).rec();                      // 7 - 3 = 4
    a.pushb(-7).pushb(2).op(0x20).rec();                     // -7 / 2 = -3
    a.pushb(-7).pushb(2).op(0x22).rec();                     // -7 % 2 = -1
    a.pushb(5).pushb(0).op(0x20).rec();                      // division by zero -> 0
    a.pushb(-1).pushb(1).op(0x28).rec();                     // -1 < 1 (signed) = 1
    a.pushb(1).pushb(3).op(0x3C).rec();                      // shl: count 1 below, value 3 on top = 6
    a.pushb(2).pushw(0x8000).op(0x3D).rec();                 // sar 0x8000 by 2 = 0xE000 (-8192)
    a.pushw(0x00F0).op(0x3B).rec();                          // bnot = 0xFF0F (-241)
    a.pushb(3).pushb(0).op(0x32).rec();                      // land 3,0 = 0
    a.pushb(3).pushb(0).op(0x34).rec();                      // lor = 1
    a.pushw(30000).pushw(30000).op(0x14).rec();              // add wraps: 60000 -> -5536
    a.pushb(-2).op(0x60).op(0x0C).u32(5).op(0x15).op(0x61).rec();  // w2d, add.d, d2w: 3
    // locals: pop.w / ld.w / pop.b / ld.b sign extension
    a.pushw(0x1234).op(0x01).u8(0xFE).op(0x3F).u8(0xFE).rec();     // [BP-2] = 0x1234
    a.pushb(-3).op(0x00).u8(0xFC).op(0x3E).u8(0xFC).rec();          // byte -3
    // globals: a 3-bit field crossing a byte boundary
    a.pushb(5).op(0x4F).u16(7).u8(3).op(0x4E).u16(7).u8(3).rec();  // 5
    a.op(0x4E).u16(8).u8(1).rec();                                  // bit 8 = (5 >> 1) & 1 = 0
    a.op(0x4E).u16(9).u8(1).rec();                                  // bit 9 = 1
    // jz / jmp: if (0) record 99 else record 42
    a.pushb(0).op(0x51).u16(11);                             // jz to the else branch
    a.pushb(99).rec().op(0x52).u16(8);                       // then-branch (8 bytes) and a jmp over the else
    a.pushb(42).rec();
    // intrinsic return value through temp
    a.pushb(21).op(0x0F).u8(2).u16(0x0FA).op(0x6E).u8(0xFE).op(0x5E).rec();  // 42
    // call / ret with an argument; the callee returns arg + 1 in temp
    unsigned call_at = a.here();
    a.pushb(9).op(0x11).u16(0x300).u16(0);                   // patched below
    a.op(0x6E).u8(0xFE).op(0x5E).rec();                      // 10
    // strings
    a.str("Hello ").str("Avatar").op(0x16).recstr();         // strcat
    a.str("ab").str("ab").op(0x26).rec();                    // streq = 1
    // lists: mklist [1,2,3]; getelem 2; foreach sum
    a.pushb(1).pushb(2).pushb(3).op(0x0E).u8(2).u8(3).op(0x01).u8(0xFE);  // [BP-2] = list
    a.op(0x3F).u8(0xFE).pushb(2).op(0x44).u8(2).u8(0).rec();           // element 2 = 2
    a.pushb(0).op(0x01).u8(0xFC);                                       // [BP-4] = 0 (sum)
    unsigned loop_top = a.here();
    a.op(0x42).u8(0xFE).u8(2).pushw(0xFFFF);
    unsigned fe = a.here();
    a.op(0x75).u8(0xFA).u8(2).u16(0);                                   // foreach var [BP-6]
    unsigned body = a.here();
    a.op(0x3F).u8(0xFC).op(0x3F).u8(0xFA).op(0x14).op(0x01).u8(0xFC);   // sum += x
    a.op(0x52).u16(0);                                                  // jmp back to foreach
    unsigned after = a.here();
    // patch the foreach exit and the jmp back
    {
        size_t fo = fe - 0x80;
        int rel = int(after) - int(fe + 5);
        a.b[fo + 3] = uint8_t(rel); a.b[fo + 4] = uint8_t(rel >> 8);
        size_t jo = after - 3 - 0x80;
        int jrel = int(fe) - int(after);
        a.b[jo + 1] = uint8_t(jrel); a.b[jo + 2] = uint8_t(jrel >> 8);
    }
    (void)loop_top; (void)body;
    a.op(0x3F).u8(0xFC).rec();                                          // 6
    // inlist: 3 in [1,2,3] -> 1 (frees the copy)
    a.pushb(3).op(0x42).u8(0xFE).u8(2).op(0x38).u8(2).u8(0).rec();
    a.op(0x64).u8(0xFE);                                                // free.list
    // lea / deref / store
    a.pushw(0x0BEE).op(0x01).u8(0xFE);
    a.op(0x4B).u8(0xFE).op(0x4C).u8(2).rec();                           // deref 2 = 0x0BEE
    // search loop over the fake host's ids, with id 6 destroyed: visits 5 and 7
    a.op(0x74).u8('$');                                                 // criteria "$"
    a.pushw(0x100).pushw(1);                                            // B = range, A = item
    a.op(0x70).u8(0xFE).u8(1).u8(2);                                    // loop var [BP-2], len 1, type 2
    unsigned lp = a.here();
    a.op(0x51).u16(0);                                                  // jz exit (patched)
    a.op(0x3F).u8(0xFE).rec();
    a.op(0x73).op(0x52).u16(0);                                         // loopnext; jmp lp
    unsigned lexit = a.here();
    {
        size_t jzo = lp - 0x80;
        int rel = int(lexit) - int(lp + 3);
        a.b[jzo + 1] = uint8_t(rel); a.b[jzo + 2] = uint8_t(rel >> 8);
        size_t jo = lexit - 3 - 0x80;
        int jrel = int(lp) - int(lexit);
        a.b[jo + 1] = uint8_t(jrel); a.b[jo + 2] = uint8_t(jrel >> 8);
    }
    a.op(0x6E).u8(uint8_t(-0x34));                                      // addsp -AREA(2)
    a.pushb(77).rec();                                                  // stack is balanced again
    a.op(0x50).op(0x79);
    unsigned callee = a.here();
    a.op(0x5A).u8(0).op(0x3F).u8(6).pushb(1).op(0x14).op(0x12).op(0x50).op(0x79);  // return arg+1
    {
        size_t co = call_at - 0x80 + 2;  // after push.sb 9
        a.b[co + 1] = uint8_t(0x300 & 0xFF); a.b[co + 2] = uint8_t(0x300 >> 8);
        a.b[co + 3] = uint8_t(callee); a.b[co + 4] = uint8_t(callee >> 8);
    }

    VmRig r({{0x300, {a.b, {{1, 0x80}}}}});
    CHECK(r.ok);
    r.host.alive = {5, 7};
    r.vm->trace = getenv("UCTRACE") != nullptr;
    uint16_t pid = r.vm->start(0x300, 0x80, false, 0, {}, false);
    CHECK(pid != 0);
    r.run(2);
    const int expect[] = {4, -3, -1, 0, 1, 6, -8192, -241, 0, 1, -5536, 3, 0x1234, -3, 5, 0, 1, 42, 42, 10,
                          1, 2, 6, 1, 0x0BEE, 5, 7, 77};
    CHECK_EQ(r.rec.size(), sizeof expect / sizeof expect[0]);
    for (size_t i = 0; i < r.rec.size() && i < sizeof expect / sizeof expect[0]; ++i)
        if (r.rec[i] != expect[i]) { std::printf("  vm record %zu: got %d want %d\n", i, r.rec[i], expect[i]); CHECK(false); }
    CHECK(r.recs.size() == 1 && r.recs[0] == "Hello Avatar");
    CHECK_EQ(r.vm->errors(), 0u);
    CHECK_EQ(r.host.types.size(), 1u);
    CHECK(r.kernel.get(pid) == nullptr);  // returned from the outermost frame and was removed
    CHECK_EQ(r.vm->list_count(), 0u);     // every list was freed

    // --- processes: spawn runs the child at once; implies waits; push.pres reads the result.
    Asm p;
    p.op(0x5A).u8(0);
    p.op(0x59);                                                   // B = my pid
    p.op(0x0C).u32(0).op(0x57).u8(0).u8(0).u16(0x301).u16(0);     // spawn child (patched addr)
    unsigned spawn_at = p.here() - 7;
    p.op(0x5E);                                                   // A = child pid
    p.op(0x54).u8(1).u8(1).op(0x12).op(0x53);                     // implies; pop.tmp; suspend
    p.pushb(2).rec();                                             // after the child ended
    p.op(0x6D).op(0x61).rec();                                    // child's result (d2w)
    p.op(0x50).op(0x79);
    unsigned child = p.here();
    p.op(0x5A).u8(0).pushb(1).rec();                              // first slice: nested in the parent
    p.op(0x53).op(0x53);                                          // two frames
    p.pushb(5).op(0x60).op(0x08);                                 // result = 5
    p.op(0x50).op(0x79);
    {
        size_t so = spawn_at - 0x80;
        p.b[so + 5] = uint8_t(child); p.b[so + 6] = uint8_t(child >> 8);
        p.b[so + 3] = uint8_t(0x300 & 0xFF); p.b[so + 4] = uint8_t(0x300 >> 8);
    }
    // exclude: a handler with setinfo(item 9, type 0x207) + exclude, started twice.
    Asm e;
    e.op(0x5A).u8(0).pushw(0x207).pushb(9).op(0x77).op(0x78);
    e.pushb(3).rec().op(0x53).op(0x50).op(0x79);
    VmRig r2({{0x300, {p.b, {}}}, {0x301, {e.b, {{1, 0x80}}}}});
    r2.vm->start(0x300, 0x80, false, 0, {}, false);
    r2.kernel.frame();
    CHECK(r2.rec.size() >= 1 && r2.rec[0] == 1);  // the child's first slice ran inside the parent's turn
    CHECK_EQ(r2.rec.size(), 1u);                  // the parent is waiting
    r2.run(4);
    CHECK_EQ(r2.rec.size(), 3u);
    if (r2.rec.size() == 3) { CHECK_EQ(r2.rec[1], 2); CHECK_EQ(r2.rec[2], 5); }
    r2.rec.clear();
    r2.vm->start_event(0x301, 1, 9);
    r2.vm->start_event(0x301, 1, 9);
    r2.run(2);
    CHECK_EQ(r2.rec.size(), 1u);  // the second copy excluded itself
    CHECK_EQ(r2.vm->errors(), 0u);

    // --- the event frame: [BP+6] is the this pointer, deref 2 gives the item id.
    Asm t;
    t.op(0x5A).u8(0).op(0x40).u8(6).op(0x4C).u8(2).rec().op(0x3F).u8(10).rec().op(0x50).op(0x79);
    VmRig r3({{0x302, {t.b, {{6, 0x80}}}}});
    r3.vm->start_event(0x302, 6, 1234, {0x2A, 0x00, 0x00, 0x00});
    r3.run(1);
    CHECK(r3.rec.size() == 2 && r3.rec[0] == 1234 && r3.rec[1] == 42);
    CHECK_EQ(r3.vm->start_event(0x302, 1, 1), 0);  // no handler for event 1

    // --- unknown intrinsic: logged once, temp = 0, the script goes on
    Asm u;
    u.op(0x5A).u8(0).op(0x0F).u8(0).u16(0x0FF).op(0x5E).rec().op(0x50).op(0x79);
    VmRig r4({{0x303, {u.b, {}}}});
    r4.vm->start(0x303, 0x80, false, 0, {}, false);
    r4.run(1);
    CHECK(r4.rec.size() == 1 && r4.rec[0] == 0);
    CHECK_EQ(r4.vm->unimplemented_hit().size(), 1u);
}

// ---------------------------------------------------------------------------------------------
// EUSECODE.FLX: every class loads and decodes

void test_usecode_data(const GameData& d) {
    const UsecodeFile& uc = d.usecode;
    CHECK_EQ(uc.existing_classes(), 516u);
    CHECK(uc.class_name(1025) == "AVATAR");
    CHECK(uc.class_name(1187) == "FIRST");
    CHECK(uc.class_name(562) == "MUSIC");
    CHECK(uc.class_name(68) == "DOOR_NS");
    CHECK_EQ(uc.event_entry(68, 0), 0x80u);
    CHECK_EQ(uc.event_entry(68, 1), 0x98u);
    CHECK_EQ(uc.event_entry(68, 6), 0xB4u);
    CHECK_EQ(uc.event_entry(1025, 4), 0x3666u);
    CHECK_EQ(uc.event_entry(1187, 7), 0x80u);
    // Linear decode of all classes: only known opcodes, ending exactly on the last byte,
    // every event entry on an instruction boundary; count enters, ends, intrinsic numbers.
    unsigned bad_classes = 0, enters = 0, ends = 0, bad_entries = 0, mask_mismatch = 0;
    std::set<unsigned> intrinsics;
    uint64_t calli = 0;
    for (unsigned c = 0; c < uc.class_count(); ++c) {
        uint32_t size = uc.code_size(c);
        if (!size) continue;
        const uint8_t* code = uc.code(c);
        std::vector<bool> start(size + 1, false);
        uint32_t ip = 0x80;
        bool ok = true;
        while (ip < size) {
            unsigned len = uc_op_length(code + ip, size - ip);
            if (!len) { ok = false; break; }
            start[ip] = true;
            if (code[ip] == 0x5A) ++enters;
            if (code[ip] == 0x79) ++ends;
            if (code[ip] == 0x0F) { intrinsics.insert(rd16(code + ip + 2)); ++calli; }
            ip += len;
        }
        if (!ok || ip != size) ++bad_classes;
        uint32_t mask = 0;
        for (unsigned e = 0; e < UsecodeFile::kEvents; ++e) {
            uint32_t en = uc.event_entry(c, e);
            if (!en) continue;
            mask |= 1u << e;
            if (en >= size || !start[en] || code[en] != 0x5A) ++bad_entries;
        }
        if (mask != uc.event_mask(c)) ++mask_mismatch;
    }
    CHECK_EQ(bad_classes, 0u);
    CHECK_EQ(bad_entries, 0u);
    CHECK_EQ(mask_mismatch, 0u);
    CHECK_EQ(enters, 1575u);
    CHECK_EQ(ends, 1555u);
    CHECK_EQ(intrinsics.size(), 182u);
    CHECK_EQ(calli, 13952u);
    // Intrinsic coverage of what v2.10 calls.
    World w(d);
    unsigned covered = 0;
    std::vector<unsigned> missing;
    for (unsigned n : intrinsics) {
        if (w.vm().has_intrinsic(n)) ++covered;
        else missing.push_back(n);
    }
    std::printf("  intrinsics: %u of the %zu used by v2.10 implemented (%u registered in all); missing:",
                covered, intrinsics.size(), w.vm().implemented_count());
    for (unsigned n : missing) std::printf(" %03X", n);
    std::printf("\n");
    CHECK(covered >= 180);
}

// ---------------------------------------------------------------------------------------------
// A headless new game

void test_game(const GameData& d) {
    World w(d);
    w.auto_ui = true;
    w.new_game();
    CHECK(w.avatar() != nullptr);
    CHECK_EQ(w.map().map_number(), 3u);
    CHECK(w.stasis);
    CHECK_EQ(w.objects().actor(1)->x, 14527);
    // The backpack is in the Avatar.
    const Actor* av = w.avatar();
    CHECK(av->contents.size() == 1 && w.item(av->contents[0]) && w.item(av->contents[0])->shape == 529);
    // Permanent NPCs: 61, with NPC usecode classes.
    unsigned npcs = 0;
    for (unsigned n = 1; n < 256; ++n)
        if (const Actor* a = w.actor(uint16_t(n)); a && (a->ext_flags & kExtPermanentNpc)) ++npcs;
    CHECK_EQ(npcs, 61u);
    CHECK_EQ(w.usecode_class(*w.actor(2)), 1026);
    // The FIRST egg's class.
    const Item* first = w.map().find_item(73, 36, 16188, 7500, 256);
    CHECK(first != nullptr);
    if (first) CHECK_EQ(w.usecode_class(*first), 0x4A3);

    // Map-item animation (TYPEFLAG anim types 1-6) steps items in the fast area.
    w.frame();
    std::map<uint16_t, uint16_t> anim_frames;
    for (int cy = 0; cy < CurrentMap::kChunks; ++cy)
        for (int cx = 0; cx < CurrentMap::kChunks; ++cx)
            if (w.map().is_fast(cx, cy))
                for (uint16_t id : w.map().chunk(cx, cy))
                    if (const Item* it = w.item(id); it && id >= 256) {
                        unsigned t = d.typeflags.get(it->shape).anim_type();
                        if (t == 1 || t == 3) anim_frames[id] = it->frame;
                    }
    for (int i = 0; i < 60; ++i) w.frame();
    unsigned changed = 0;
    for (auto& [id, f] : anim_frames)
        if (const Item* it = w.item(id); it && it->frame != f) ++changed;
    std::printf("  map-item animation: %u of %zu looping items in the fast area changed frame in 2 s\n", changed,
                anim_frames.size());
    CHECK(!anim_frames.empty());
    CHECK(changed > 0);

    // Run 1 minute of game time: FIRST hatches, the opening conversation runs.
    for (int i = 0; i < 1739; ++i) w.frame();
    CHECK_EQ(w.startup_state, 1u);
    CHECK_EQ(w.vm().errors(), 0u);
    CHECK(w.events_started > 5);
    CHECK(w.vm().instructions() > 2000);
    // Devon (NPC 2) is talking or has talked; the Avatar got up (frame changed from lying).
    CHECK(w.kernel().frame_number() == 1800);
    // Keep going through the whole conversation and the first schedule hour (27000 frames).
    bool stasis_released = false;
    unsigned released_at = 0;
    for (int i = 0; i < 28000; ++i) {
        w.frame();
        if (!w.stasis && !stasis_released) { stasis_released = true; released_at = w.kernel().frame_number(); }
    }
    CHECK(stasis_released);
    CHECK_EQ(w.vm().errors(), 0u);
    std::printf("  headless new game: %u frames, %llu usecode instructions, %llu events, %zu processes, "
                "stasis released at frame %u, %u VM errors, %zu unimplemented intrinsics hit\n",
                w.kernel().frame_number(), (unsigned long long)w.vm().instructions(),
                (unsigned long long)w.events_started, w.kernel().size(), released_at, w.vm().errors(),
                w.vm().unimplemented_hit().size());
    CHECK_EQ(w.vm().unimplemented_hit().size(), 0u);
    CHECK(w.camera_followed() == 1);

    // Map change: teleport the Avatar to Tenebrae and run a little.
    w.teleport_actor(1, 16300, 12300, 48, 39);
    CHECK_EQ(w.map().map_number(), 39u);
    CHECK(w.avatar()->chunk >= 0);
    for (int i = 0; i < 600; ++i) w.frame();
    CHECK_EQ(w.vm().errors(), 0u);
    unsigned fast_actors = 0;
    for (unsigned n = 1; n < 256; ++n)
        if (const Actor* a = w.actor(uint16_t(n)); a && (a->flags & kItemFastArea)) ++fast_actors;
    CHECK(fast_actors >= 3);
}
