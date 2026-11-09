// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The usecode interpreter (specs/usecode/vm.md, opcodes.md, loops.md).
#include "usecode/uc_machine.h"

#include <cstdlib>
#include "usecode/uc_patches.h"

#include <algorithm>
#include <cstring>

#include "core/bytes.h"
#include "core/log.h"
#include "usecode/usecode_file.h"

namespace pg {

namespace {
const std::string kEmpty;

// Loop area sizes by search type (loops.md section 1).
unsigned loop_area(unsigned type) {
    switch (type) {
    case 2: case 3: return 0x34;
    case 4: return 0x28;
    case 5: return 0x2A;
    case 6: return 0x3D;
    default: return 0;
    }
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Instruction lengths (opcodes.md). 0 = not an opcode.

unsigned uc_op_length(const uint8_t* p, size_t remain) {
    if (remain == 0) return 0;
    unsigned n = 0;
    switch (p[0]) {
    case 0x00: case 0x01: case 0x02: n = 2; break;
    case 0x03: n = 3; break;
    case 0x08: n = 1; break;
    case 0x09: n = 4; break;
    case 0x0A: n = 2; break;
    case 0x0B: n = 3; break;
    case 0x0C: n = 5; break;
    case 0x0D:
        if (remain < 3) return 0;
        n = 4 + rd16(p + 1);
        break;
    case 0x0E: n = 3; break;
    case 0x0F: n = 4; break;
    case 0x11: n = 5; break;
    case 0x12: case 0x13: n = 1; break;
    case 0x14: case 0x15: case 0x16: case 0x17: n = 1; break;
    case 0x19: case 0x1A: case 0x1B: n = 2; break;
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C:
    case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
    case 0x35: case 0x36: case 0x37: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D:
        n = 1; break;
    case 0x38: n = 3; break;
    case 0x3E: case 0x3F: case 0x40: case 0x41: n = 2; break;
    case 0x42: n = 3; break;
    case 0x43: n = 2; break;
    case 0x44: n = 3; break;
    case 0x45: n = 3; break;
    case 0x4B: n = 2; break;
    case 0x4C: case 0x4D: n = 2; break;
    case 0x4E: case 0x4F: n = 4; break;
    case 0x50: n = 1; break;
    case 0x51: case 0x52: n = 3; break;
    case 0x53: n = 1; break;
    case 0x54: n = 3; break;
    case 0x57: n = 7; break;
    case 0x58: n = 9; break;
    case 0x59: n = 1; break;
    case 0x5A: n = 2; break;
    case 0x5B: n = 3; break;
    case 0x5C: n = 12; break;
    case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: n = 1; break;
    case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: n = 2; break;
    case 0x69: n = 2; break;
    case 0x6B: n = 1; break;
    case 0x6C: n = 3; break;
    case 0x6D: n = 1; break;
    case 0x6E: case 0x6F: n = 2; break;
    case 0x70: n = 4; break;
    case 0x73: n = 1; break;
    case 0x74: n = 2; break;
    case 0x75: case 0x76: n = 5; break;
    case 0x77: case 0x78: case 0x79: case 0x7A: n = 1; break;
    default: return 0;
    }
    return n <= remain ? n : 0;
}

// ---------------------------------------------------------------------------------------------
// Processes

UCProcess::UCProcess(UCMachine& m, uint16_t c, uint16_t addr, bool has_this, std::vector<uint8_t> this_bytes,
                     std::vector<uint8_t> args)
    : start_cls(c), start_addr(addr), has_this_(has_this), this_bytes_(std::move(this_bytes)),
      args_(std::move(args)), m_(m) {}

void UCProcess::on_added() {
    // vm.md 4.4: this word, argument block, this pointer, then the frame (FFFF, FFFF, 0).
    uint32_t T = 0;
    if (has_this_) {
        sp -= uint32_t(this_bytes_.size());
        std::memcpy(&stack[sp], this_bytes_.data(), this_bytes_.size());
        T = sp;
    }
    sp -= uint32_t(args_.size());
    if (!args_.empty()) std::memcpy(&stack[sp], args_.data(), args_.size());
    auto push16 = [&](uint16_t v) { sp -= 2; stack[sp] = uint8_t(v); stack[sp + 1] = uint8_t(v >> 8); };
    if (has_this_) {
        uint32_t ptr = make_ptr(pid(), uint16_t(T));
        push16(uint16_t(ptr >> 16));
        push16(uint16_t(ptr));
    }
    push16(0xFFFF);  // caller's class
    push16(0xFFFF);  // caller's IP
    push16(0);       // caller's BP
    bp = uint16_t(sp);
    cls = start_cls;
    ip = start_addr;
    this_bytes_.clear();
    args_.clear();
}

void UCProcess::run() { m_.execute(*this); }

void UCProcess::save(SaveWriter& out) const {
    out.u16(cls); out.u16(ip); out.u16(bp); out.u32(sp); out.u32(temp);
    out.u16(start_cls); out.u16(start_addr);
    out.bytes(stack.data() + sp, kStackSize - sp);
    out.u32(uint32_t(free_on_exit.size()));
    for (auto& [id, kind] : free_on_exit) { out.u16(id); out.u8(uint32_t(kind)); }
}

void UCProcess::load(SaveReader& in) {
    cls = uint16_t(in.u16()); ip = uint16_t(in.u16()); bp = uint16_t(in.u16());
    sp = in.u32(); temp = in.u32();
    start_cls = uint16_t(in.u16()); start_addr = uint16_t(in.u16());
    std::vector<uint8_t> st = in.bytes();
    if (sp > kStackSize || st.size() != kStackSize - sp) { in.set_fail(); return; }
    std::memcpy(stack.data() + sp, st.data(), st.size());
    free_on_exit.clear();
    unsigned n = in.u32();
    for (unsigned i = 0; i < n && !in.fail(); ++i) {
        uint16_t id = uint16_t(in.u16());
        int kind = int(in.u8());
        free_on_exit.emplace_back(id, kind);
    }
    has_this_ = false;
    this_bytes_.clear();
    args_.clear();
}

void UCMachine::save(SaveWriter& out) const {
    out.tag("UCVM");
    out.raw(globals_.data(), globals_.size());
    std::vector<uint16_t> ids;
    for (auto& [id, str] : strings_) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    out.u32(uint32_t(ids.size()));
    for (uint16_t id : ids) { out.u16(id); out.str(strings_.at(id)); }
    ids.clear();
    for (auto& [id, l] : lists_) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    out.u32(uint32_t(ids.size()));
    for (uint16_t id : ids) {
        const UCList& l = lists_.at(id);
        out.u16(id); out.u8(l.elem); out.bytes(l.data.data(), l.data.size());
    }
    string_ids_.save(out);
    list_ids_.save(out);
}

bool UCMachine::load(SaveReader& in) {
    reset();
    if (!in.tag("UCVM")) return false;
    in.raw(globals_.data(), globals_.size());
    unsigned n = in.u32();
    for (unsigned i = 0; i < n && !in.fail(); ++i) {
        uint16_t id = uint16_t(in.u16());
        strings_[id] = in.str();
    }
    n = in.u32();
    for (unsigned i = 0; i < n && !in.fail(); ++i) {
        uint16_t id = uint16_t(in.u16());
        UCList l;
        l.elem = in.u8();
        l.data = in.bytes();
        lists_[id] = std::move(l);
    }
    string_ids_.load(in);
    list_ids_.load(in);
    return !in.fail();
}

void UCProcess::on_terminate() {
    for (auto& [id, kind] : free_on_exit) {
        if (kind == 1) m_.free_string(id);
        else if (kind == 2) m_.free_slist(id);
        else if (kind == 3) m_.free_list(id);
    }
    free_on_exit.clear();
}

// ---------------------------------------------------------------------------------------------
// Intrinsic argument helpers

uint16_t IntrinsicCall::obj(unsigned off) const { return m.resolve16(u32(off)); }

std::string IntrinsicCall::str(unsigned off) const { return m.string(m.resolve16(u32(off))); }

bool IntrinsicCall::write16(unsigned off, uint16_t v) const {
    uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
    return m.write_ptr(u32(off), b, 2);
}

bool IntrinsicCall::write_bytes(unsigned off, const uint8_t* b, unsigned n) const { return m.write_ptr(u32(off), b, n); }

// ---------------------------------------------------------------------------------------------
// Machine

UCMachine::UCMachine(const UsecodeFile& file, Kernel& kernel)
    : file_(file), kernel_(kernel), intr_(kIntrinsics), intr_names_(kIntrinsics), intr_calls_(kIntrinsics, 0) {}

void UCMachine::reset() {
    globals_.fill(0);
    strings_.clear();
    lists_.clear();
    string_ids_.reset(1, 65534);
    list_ids_.reset(1, 65534);
    unimpl_hit_.clear();
    errors_ = 0;
    std::fill(intr_calls_.begin(), intr_calls_.end(), 0);
}

void UCMachine::set_intrinsic(unsigned n, IntrinsicFn fn, const char* name) {
    if (n >= kIntrinsics) return;
    intr_[n] = std::move(fn);
    if (name) intr_names_[n] = name;
}

unsigned UCMachine::implemented_count() const {
    unsigned n = 0;
    for (auto& f : intr_) n += f ? 1 : 0;
    return n;
}

uint16_t UCMachine::start(uint16_t cls, uint16_t addr, bool has_this, uint16_t this_id,
                          const std::vector<uint8_t>& args, bool run_now) {
    std::vector<uint8_t> tb;
    if (has_this) tb = {uint8_t(this_id), uint8_t(this_id >> 8)};
    auto p = std::make_unique<UCProcess>(*this, cls, addr, has_this, std::move(tb), args);
    return run_now ? kernel_.add_and_run(std::move(p)) : kernel_.add(std::move(p));
}

uint16_t UCMachine::start_event(uint16_t cls, unsigned e, uint16_t item, const std::vector<uint8_t>& args) {
    uint32_t entry = file_.event_entry(cls, e);
    if (!entry) return 0;
    return start(cls, uint16_t(entry), true, item, args, false);
}

uint16_t UCMachine::get_global(unsigned pos, unsigned n) const {
    uint16_t v = 0;
    for (unsigned i = 0; i < n; ++i) {
        unsigned b = pos + i;
        if (b / 8 < globals_.size() && (globals_[b / 8] >> (b % 8) & 1)) v |= uint16_t(1u << i);
    }
    return v;
}

void UCMachine::set_global(unsigned pos, unsigned n, uint16_t v) {
    for (unsigned i = 0; i < n; ++i) {
        unsigned b = pos + i;
        if (b / 8 >= globals_.size()) break;
        if (v >> i & 1) globals_[b / 8] = uint8_t(globals_[b / 8] | (1u << (b % 8)));
        else globals_[b / 8] = uint8_t(globals_[b / 8] & ~(1u << (b % 8)));
    }
}

uint16_t UCMachine::new_string(const std::string& s) {
    uint16_t id = string_ids_.allocate();
    if (!id) { PG_LOG("usecode: out of string ids"); return 0; }
    strings_[id] = s;
    return id;
}

const std::string& UCMachine::string(uint16_t id) const {
    auto it = strings_.find(id);
    return it == strings_.end() ? kEmpty : it->second;
}

void UCMachine::free_string(uint16_t id) {
    if (strings_.erase(id)) string_ids_.release(id);
}

uint16_t UCMachine::new_list(unsigned elem) {
    uint16_t id = list_ids_.allocate();
    if (!id) { PG_LOG("usecode: out of list ids"); return 0; }
    lists_[id].elem = elem;
    if (trace) PG_LOG("uc new list %u", id);
    return id;
}

UCList* UCMachine::list(uint16_t id) {
    auto it = lists_.find(id);
    return it == lists_.end() ? nullptr : &it->second;
}

void UCMachine::free_list(uint16_t id) {
    if (trace) PG_LOG("uc free list %u", id);
    if (lists_.erase(id)) list_ids_.release(id);
}

void UCMachine::free_slist(uint16_t id) {
    if (UCList* l = list(id))
        for (unsigned i = 0; i < l->size(); ++i) free_string(l->word(i));
    free_list(id);
}

uint16_t UCMachine::dup_list(uint16_t id, unsigned elem) {
    UCList copy;  // copy first: new_list may rehash the table
    copy.elem = elem;
    if (UCList* src = list(id)) copy = *src;
    uint16_t n = new_list(copy.elem);
    if (n) *list(n) = std::move(copy);
    return n;
}

uint16_t UCMachine::dup_slist(uint16_t id) {
    std::vector<uint16_t> words;
    if (UCList* src = list(id))
        for (unsigned i = 0; i < src->size(); ++i) words.push_back(src->word(i));
    uint16_t n = new_list(2);
    for (uint16_t w : words) {
        uint16_t s = dup_string(w);
        list(n)->push_word(s);
    }
    return n;
}

bool UCMachine::read_ptr(uint32_t ptr, uint8_t* out, unsigned n) const {
    uint16_t seg = uint16_t(ptr >> 16), off = uint16_t(ptr);
    if (seg >= 1 && seg <= 0x7FFE) {
        auto* p = dynamic_cast<UCProcess*>(kernel_.get(seg));
        if (!p || off + n > UCProcess::kStackSize) return false;
        std::memcpy(out, &p->stack[off], n);
        return true;
    }
    if (seg == kSegString || seg == kSegObject) {
        uint8_t b[4] = {uint8_t(off), uint8_t(off >> 8), 0, 0};
        if (n > 4) return false;
        std::memcpy(out, b, n);
        return true;
    }
    return false;
}

bool UCMachine::write_ptr(uint32_t ptr, const uint8_t* in, unsigned n) {
    uint16_t seg = uint16_t(ptr >> 16), off = uint16_t(ptr);
    if (seg >= 1 && seg <= 0x7FFE) {
        auto* p = dynamic_cast<UCProcess*>(kernel_.get(seg));
        if (!p || off + n > UCProcess::kStackSize) return false;
        std::memcpy(&p->stack[off], in, n);
        return true;
    }
    return false;
}

uint16_t UCMachine::resolve16(uint32_t ptr) const {
    uint8_t b[2] = {0, 0};
    if (!read_ptr(ptr, b, 2)) return 0;
    return uint16_t(b[0] | (b[1] << 8));
}

bool UCMachine::error(UCProcess& p, const char* what) {
    ++errors_;
    PG_LOG("usecode error: %s (pid %u, class %04X:%04X, item %u)", what, p.pid(), p.cls, p.ip, p.item);
    p.terminate_deferred();
    return false;
}

// ---------------------------------------------------------------------------------------------
// The interpreter

void UCMachine::execute(UCProcess& p) {
    auto& st = p.stack;
    const unsigned kMaxPerTurn = 2000000;
    unsigned executed = 0;

    // Stack helpers: return false on overflow / underflow (then the process errors).
    bool bad = false;
    auto need = [&](uint32_t n) { if (p.sp < n) { bad = true; return false; } return true; };
    auto have = [&](uint32_t n) { if (p.sp + n > UCProcess::kStackSize) { bad = true; return false; } return true; };
    auto push16 = [&](uint16_t v) { if (!need(2)) return; p.sp -= 2; st[p.sp] = uint8_t(v); st[p.sp + 1] = uint8_t(v >> 8); };
    auto push32 = [&](uint32_t v) { push16(uint16_t(v >> 16)); push16(uint16_t(v)); };
    auto pop16 = [&]() -> uint16_t {
        if (!have(2)) return 0;
        uint16_t v = uint16_t(st[p.sp] | (st[p.sp + 1] << 8));
        p.sp += 2;
        return v;
    };
    auto pop32 = [&]() -> uint32_t { uint32_t lo = pop16(); uint32_t hi = pop16(); return lo | (hi << 16); };
    auto at = [&](int off, unsigned n) -> uint8_t* {  // BP-relative
        int a = int(p.bp) + off;
        if (a < 0 || a + int(n) > int(UCProcess::kStackSize)) { bad = true; return nullptr; }
        return &st[size_t(a)];
    };
    auto rdw = [&](int off) -> uint16_t { uint8_t* q = at(off, 2); return q ? uint16_t(q[0] | (q[1] << 8)) : 0; };
    auto wrw = [&](int off, uint16_t v) { if (uint8_t* q = at(off, 2)) { q[0] = uint8_t(v); q[1] = uint8_t(v >> 8); } };
    auto sp_word = [&](int off) -> uint16_t {
        int a = int(p.sp) + off;
        if (a < 0 || a + 2 > int(UCProcess::kStackSize)) { bad = true; return 0; }
        return uint16_t(st[size_t(a)] | (st[size_t(a) + 1] << 8));
    };

    while (p.live()) {
        const uint8_t* code = file_.code(p.cls);
        uint32_t size = file_.code_size(p.cls);
        if (p.ip >= size) {  // inside code the story patches added (uc_patches.h), the option since turned Off
            const std::vector<uint8_t>& pc = file_.patched_code(p.cls);
            if (p.ip < pc.size()) { code = pc.data(); size = uint32_t(pc.size()); }
        }
        if (!code || p.ip >= size) { error(p, "IP outside class code"); return; }
        const uint8_t* ins = code + p.ip;
        unsigned len = uc_op_length(ins, size - p.ip);
        if (!len) { error(p, "unknown opcode or truncated instruction"); return; }
        if (++executed > kMaxPerTurn) { error(p, "too many instructions in one turn"); return; }
        ++instructions_;
        uint16_t next = uint16_t(p.ip + len);
        uint8_t op = ins[0];
        const uint8_t* o = ins + 1;
        if (trace) PG_LOG("uc %u %04X:%04X op %02X sp %04X", p.pid(), p.cls, p.ip, op, p.sp);
        bool stop = false;
        uint16_t jump_to = next;

        switch (op) {
        // --- stores
        case 0x00: { uint16_t v = pop16(); if (uint8_t* q = at(int8_t(o[0]), 1)) *q = uint8_t(v); break; }
        case 0x01: { uint16_t v = pop16(); wrw(int8_t(o[0]), v); break; }
        case 0x02: {
            uint32_t v = pop32();
            if (uint8_t* q = at(int8_t(o[0]), 4)) { q[0] = uint8_t(v); q[1] = uint8_t(v >> 8); q[2] = uint8_t(v >> 16); q[3] = uint8_t(v >> 24); }
            break;
        }
        case 0x03: {
            unsigned n = o[1];
            if (!have(n)) break;
            if (uint8_t* q = at(int8_t(o[0]), n)) std::memmove(q, &st[p.sp], n);
            p.sp += n;
            break;
        }
        case 0x08: p.result = pop32(); break;
        case 0x09: {  // setelem
            uint16_t lid = rdw(int8_t(o[0]));
            unsigned n = o[1];
            uint16_t idx = pop16();
            if (!have(n)) break;
            uint8_t elem[8] = {};
            std::memcpy(elem, &st[p.sp], n < 8 ? n : 8);
            p.sp += n;
            UCList* l = list(lid);
            if (!l) { error(p, "setelem: no such list"); return; }
            if (idx >= 1 && idx <= l->size() && n == l->elem) std::memcpy(&l->data[(idx - 1) * n], elem, n);
            else if (idx >= 1 && n == l->elem) {  // grow (not seen in v2.10)
                l->data.resize(size_t(idx) * n, 0);
                std::memcpy(&l->data[(idx - 1) * n], elem, n);
            }
            break;
        }
        case 0x12: p.temp = pop16(); break;
        case 0x13: p.temp = pop32(); break;

        // --- pushes
        case 0x0A: push16(uint16_t(int16_t(int8_t(o[0])))); break;
        case 0x0B: push16(rd16(o)); break;
        case 0x0C: push32(rd32(o)); break;
        case 0x0D: {
            unsigned n = rd16(o);
            if (o[2 + n] != 0) { error(p, "push.str without terminator"); return; }
            // The story patch layer's text corrections (uc_patches.h; only with the bug fixes On).
            std::string text = story_patches().fix_text(std::string(reinterpret_cast<const char*>(o + 2), n));
            if (text_hook) text_hook(p.cls, text);
            push16(new_string(text));
            break;
        }
        case 0x0E: {  // mklist size count
            unsigned sz = o[0], count = o[1];
            if (!have(sz * count)) break;
            uint16_t lid = new_list(sz);
            UCList* l = list(lid);
            for (unsigned k = 0; k < count; ++k) {  // first pushed (highest address) = element 1
                const uint8_t* e = &st[p.sp + (count - 1 - k) * sz];
                l->data.insert(l->data.end(), e, e + sz);
            }
            p.sp += sz * count;
            push16(lid);
            break;
        }
        case 0x3E: { uint8_t* q = at(int8_t(o[0]), 1); push16(q ? uint16_t(int16_t(int8_t(*q))) : 0); break; }
        case 0x3F: push16(rdw(int8_t(o[0]))); break;
        case 0x40: {
            uint8_t* q = at(int8_t(o[0]), 4);
            push32(q ? rd32(q) : 0);
            break;
        }
        case 0x41: push16(dup_string(rdw(int8_t(o[0])))); break;
        case 0x42: push16(dup_list(rdw(int8_t(o[0])), o[1])); break;
        case 0x43: push16(dup_slist(rdw(int8_t(o[0])))); break;
        case 0x44: {  // getelem size slist
            unsigned sz = o[0];
            uint16_t idx = pop16();
            uint16_t lid = pop16();
            UCList* l = list(lid);
            uint8_t elem[8] = {};
            if (l && idx >= 1 && idx <= l->size() && l->elem == sz) std::memcpy(elem, &l->data[(idx - 1) * sz], sz);
            if (o[1] && sz == 2) {
                uint16_t sid = uint16_t(elem[0] | (elem[1] << 8));
                push16(sid ? dup_string(sid) : 0);
            } else {
                if (!need(sz)) break;
                p.sp -= sz;
                std::memcpy(&st[p.sp], elem, sz);
            }
            break;
        }
        case 0x45: {
            unsigned n = o[1];
            uint8_t* q = at(int8_t(o[0]), n);
            if (!q || !need(n)) break;
            uint8_t tmp[256];
            std::memcpy(tmp, q, n);
            p.sp -= n;
            std::memcpy(&st[p.sp], tmp, n);
            break;
        }
        case 0x59: push16(p.pid()); break;
        case 0x5D: push16(uint16_t(p.temp & 0xFF)); break;
        case 0x5E: push16(uint16_t(p.temp)); break;
        case 0x5F: push32(p.temp); break;
        case 0x6D: push32(p.result); break;

        // --- arithmetic
        case 0x14: { uint16_t b = pop16(), a = pop16(); push16(uint16_t(a + b)); break; }
        case 0x15: { uint32_t b = pop32(), a = pop32(); push32(a + b); break; }
        case 0x1C: { uint16_t b = pop16(), a = pop16(); push16(uint16_t(a - b)); break; }
        case 0x1D: { uint32_t b = pop32(), a = pop32(); push32(a - b); break; }
        case 0x1E: { uint16_t b = pop16(), a = pop16(); push16(uint16_t(a * b)); break; }
        case 0x1F: { uint32_t b = pop32(), a = pop32(); push32(a * b); break; }
        case 0x20: { int16_t b = int16_t(pop16()), a = int16_t(pop16()); push16(b ? uint16_t(a / b) : 0); break; }
        case 0x21: { int32_t b = int32_t(pop32()), a = int32_t(pop32()); push32(b ? uint32_t(a / b) : 0); break; }
        case 0x22: { int16_t b = int16_t(pop16()), a = int16_t(pop16()); push16(b ? uint16_t(a % b) : 0); break; }
        case 0x23: { int32_t b = int32_t(pop32()), a = int32_t(pop32()); push32(b ? uint32_t(a % b) : 0); break; }
        case 0x24: { uint16_t b = pop16(), a = pop16(); push16(a == b); break; }
        case 0x25: { uint32_t b = pop32(), a = pop32(); push16(a == b); break; }
        case 0x26: {
            uint16_t b = pop16(), a = pop16();
            bool eq = string(a) == string(b);
            free_string(a);
            free_string(b);
            push16(eq);
            break;
        }
        case 0x28: { int16_t b = int16_t(pop16()), a = int16_t(pop16()); push16(a < b); break; }
        case 0x29: { int32_t b = int32_t(pop32()), a = int32_t(pop32()); push16(a < b); break; }
        case 0x2A: { int16_t b = int16_t(pop16()), a = int16_t(pop16()); push16(a <= b); break; }
        case 0x2B: { int32_t b = int32_t(pop32()), a = int32_t(pop32()); push16(a <= b); break; }
        case 0x2C: { int16_t b = int16_t(pop16()), a = int16_t(pop16()); push16(a > b); break; }
        case 0x2D: { int32_t b = int32_t(pop32()), a = int32_t(pop32()); push16(a > b); break; }
        case 0x2E: { int16_t b = int16_t(pop16()), a = int16_t(pop16()); push16(a >= b); break; }
        case 0x2F: { int32_t b = int32_t(pop32()), a = int32_t(pop32()); push16(a >= b); break; }
        case 0x30: push16(pop16() == 0); break;
        case 0x31: push16(pop32() == 0); break;
        case 0x32: { uint16_t b = pop16(), a = pop16(); push16(a && b); break; }
        case 0x33: { uint32_t b = pop32(), a = pop32(); push32(a && b); break; }
        case 0x34: { uint16_t b = pop16(), a = pop16(); push16(a || b); break; }
        case 0x35: { uint32_t b = pop32(), a = pop32(); push32(a || b); break; }
        case 0x36: { uint16_t b = pop16(), a = pop16(); push16(a != b); break; }
        case 0x37: { uint32_t b = pop32(), a = pop32(); push16(a != b); break; }
        case 0x39: { uint16_t b = pop16(), a = pop16(); push16(a & b); break; }
        case 0x3A: { uint16_t b = pop16(), a = pop16(); push16(a | b); break; }
        case 0x3B: push16(uint16_t(~pop16())); break;
        case 0x3C: { uint16_t v = pop16(), c = pop16(); push16(c < 16 ? uint16_t(v << c) : 0); break; }
        case 0x3D: { int16_t v = int16_t(pop16()); uint16_t c = pop16(); push16(uint16_t(v >> (c < 15 ? c : 15))); break; }
        case 0x60: push32(uint32_t(int32_t(int16_t(pop16())))); break;
        case 0x61: push16(uint16_t(pop32())); break;

        // --- strings and lists
        case 0x16: {
            uint16_t b = pop16(), a = pop16();
            if (a == 0 || !has_string(a)) { free_string(b); error(p, "strcat onto no string"); return; }
            strings_[a] += string(b);
            free_string(b);
            push16(a);
            break;
        }
        case 0x17: {
            uint16_t b = pop16(), a = pop16();
            UCList* la = list(a);
            UCList* lb = list(b);
            if (la && lb) {
                if (la->elem != lb->elem) { error(p, "lappend: element sizes differ"); return; }
                la->data.insert(la->data.end(), lb->data.begin(), lb->data.end());
                free_list(b);
                push16(a);
            } else {
                push16(la ? a : lb ? b : 0);
            }
            break;
        }
        case 0x19: {  // sl.union
            uint16_t b = pop16(), a = pop16();
            UCList* la = list(a);
            UCList* lb = list(b);
            if (!la || !lb) { error(p, "sl.union: no such list"); return; }
            std::vector<uint16_t> add = {};
            for (unsigned i = 0; i < lb->size(); ++i) add.push_back(lb->word(i));
            for (uint16_t s : add) {
                bool dup = false;
                for (unsigned i = 0; i < la->size() && !dup; ++i) dup = string(la->word(i)) == string(s);
                if (dup) free_string(s);
                else la->push_word(s);
            }
            free_list(b);
            push16(a);
            break;
        }
        case 0x1A: {  // sl.sub
            uint16_t b = pop16(), a = pop16();
            UCList* la = list(a);
            UCList* lb = list(b);
            if (la && lb) {
                UCList keep;
                for (unsigned i = 0; i < la->size(); ++i) {
                    uint16_t s = la->word(i);
                    bool rm = false;
                    for (unsigned k = 0; k < lb->size() && !rm; ++k) rm = string(s) == string(lb->word(k));
                    if (rm) free_string(s);
                    else keep.push_word(s);
                }
                la->data = std::move(keep.data);
            }
            free_slist(b);
            push16(a);
            break;
        }
        case 0x1B: {  // l.sub
            uint16_t b = pop16(), a = pop16();
            UCList* la = list(a);
            UCList* lb = list(b);
            if (!la || !lb) { error(p, "l.sub: no such list"); return; }
            std::vector<uint8_t> keep;
            unsigned e = la->elem;
            for (unsigned i = 0; i < la->size(); ++i) {
                bool rm = false;
                for (unsigned k = 0; k < lb->size() && !rm; ++k)
                    rm = lb->elem == e && std::memcmp(&la->data[i * e], &lb->data[k * e], e) == 0;
                if (!rm) keep.insert(keep.end(), la->data.begin() + i * e, la->data.begin() + (i + 1) * e);
            }
            la->data = std::move(keep);
            free_list(b);
            push16(a);
            break;
        }
        case 0x38: {  // inlist size slist
            unsigned sz = o[0];
            uint16_t lid = pop16();
            if (!have(sz)) break;
            uint8_t val[8] = {};
            std::memcpy(val, &st[p.sp], sz < 8 ? sz : 8);
            p.sp += sz;
            UCList* l = list(lid);
            if (!l) { error(p, "inlist: no such list"); return; }
            bool found = false;
            for (unsigned i = 0; i < l->size() && !found; ++i) {
                if (o[1]) found = string(l->word(i)) == string(uint16_t(val[0] | (val[1] << 8)));
                else found = l->elem == sz && std::memcmp(&l->data[i * sz], val, sz) == 0;
            }
            if (o[1]) free_slist(lid);
            else free_list(lid);
            push16(found);
            break;
        }
        case 0x62: free_string(rdw(int8_t(o[0]))); break;
        case 0x63: free_slist(rdw(int8_t(o[0]))); break;
        case 0x64: free_list(rdw(int8_t(o[0]))); break;
        case 0x65: free_string(sp_word(int8_t(o[0]))); break;
        case 0x66: free_list(sp_word(int8_t(o[0]))); break;
        case 0x67: free_slist(sp_word(int8_t(o[0]))); break;
        case 0x69: push32(make_ptr(kSegString, rdw(int8_t(o[0])))); break;
        case 0x6B: push32(make_ptr(kSegString, pop16())); break;
        case 0x6C: {  // adopt
            int off = int8_t(o[0]);
            uint16_t id = rdw(off), copy = 0;
            switch (o[1]) {
            case 1: copy = dup_string(id); break;
            case 2: copy = list(id) ? dup_slist(id) : 0; break;
            case 3: copy = list(id) ? dup_list(id, 2) : 0; break;
            default: error(p, "adopt: bad kind"); return;
            }
            wrw(off, copy);
            if (copy) p.free_on_exit.push_back({copy, o[1]});
            break;
        }

        // --- pointers and SP
        case 0x4B: push32(make_ptr(p.pid(), uint16_t(int(p.bp) + int8_t(o[0])))); break;
        case 0x4C: {
            unsigned n = o[0];
            uint32_t ptr = pop32();
            uint8_t b[8] = {};
            if (n > 8 || !read_ptr(ptr, b, n)) { error(p, "deref: bad pointer"); return; }
            if (!need(n)) break;
            p.sp -= n;
            std::memcpy(&st[p.sp], b, n);
            break;
        }
        case 0x4D: {
            unsigned n = o[0];
            uint32_t ptr = pop32();
            if (!have(n)) break;
            if (!write_ptr(ptr, &st[p.sp], n)) { error(p, "store: bad pointer"); return; }
            p.sp += n;
            break;
        }
        case 0x6E: {
            int k = int8_t(o[0]);
            int nsp = int(p.sp) - k;
            if (nsp < 0 || nsp > int(UCProcess::kStackSize)) { bad = true; break; }
            p.sp = uint32_t(nsp);
            break;
        }
        case 0x6F: push32(make_ptr(p.pid(), uint16_t(int(p.sp) - int8_t(o[0])))); break;

        // --- globals
        case 0x4E: {
            uint16_t v = get_global(rd16(o), o[2]);
            if (global_watch) global_watch(p.cls, rd16(o), o[2], false, v);
            push16(v);
            break;
        }
        case 0x4F: {
            uint16_t v = pop16();
            if (global_watch) global_watch(p.cls, rd16(o), o[2], true, v);
            set_global(rd16(o), o[2], v);
            break;
        }

        // --- control flow
        case 0x50: {
            p.sp = p.bp;
            p.bp = pop16();
            uint16_t rip = pop16();
            uint16_t rcls = pop16();
            if (rcls == 0xFFFF && rip == 0xFFFF) {
                p.terminate_deferred();
                stop = true;
                break;
            }
            p.cls = rcls;
            jump_to = rip;
            break;
        }
        case 0x51: { uint16_t v = pop16(); if (v == 0) jump_to = uint16_t(next + rds16(o)); break; }
        case 0x52: jump_to = uint16_t(next + rds16(o)); break;
        case 0x5A: {
            unsigned n = (o[0] + 1u) & ~1u;
            if (!need(n)) break;
            p.sp -= n;
            std::memset(&st[p.sp], 0, n);
            break;
        }
        case 0x5B: case 0x5C: break;
        case 0x79: case 0x7A: error(p, "fell off the end of a function"); return;

        // --- calls
        case 0x0F: {
            unsigned argc = o[0];
            uint16_t num = rd16(o + 1);
            if (!have(argc)) break;
            IntrinsicCall call{*this, p, &st[p.sp], argc, num};
            if (num < kIntrinsics) ++intr_calls_[num];
            if (trace_intrinsics && num < kIntrinsics)
                PG_LOG("uc %u class %04X:%04X intrinsic %03X %s", p.pid(), p.cls, p.ip, num,
                       intr_names_[num].empty() ? "?" : intr_names_[num].c_str());
            if (num < kIntrinsics && intr_[num]) {
                p.temp = intr_[num](call);
                if (trace_intrinsics && std::getenv("PG_UC_ARGS")) {  // the argument words and the result
                    std::string a;
                    for (unsigned k = 0; k + 1 < argc + 1 && k < 16; k += 2) a += " " + std::to_string(st[p.sp + k] | st[p.sp + k + 1] << 8);
                    PG_LOG("uc   args%s -> %u (this %u)", a.c_str(), unsigned(p.temp), argc >= 4 ? unsigned(resolve16(call.u32(0))) : 0u);
                }
                if (p.suspended() && p.live()) {  // suspended directly (the slider, ui.md 0x04B)
                    p.ip = next;
                    return;
                }
            } else {
                if (unimpl_hit_.insert(num).second)
                    PG_LOG("usecode: intrinsic %03X not implemented (class %04X:%04X)", num, p.cls, p.ip);
                p.temp = 0;
            }
            break;
        }
        case 0x11: {
            push16(p.cls);
            push16(next);
            push16(p.bp);
            p.bp = uint16_t(p.sp);
            p.cls = rd16(o);
            jump_to = rd16(o + 2);
            break;
        }

        // --- processes
        case 0x53: stop = true; break;
        case 0x54: {
            uint16_t a = pop16(), b = pop16();
            push16(a);
            if (a != 0) {
                Process* pa = kernel_.get(a);
                Process* pb = kernel_.get(b);
                if (!pa) { error(p, "implies: no such process"); return; }
                if (pb) pb->wait_for(a);
            }
            break;
        }
        case 0x57: {  // spawn argc thissize class addr
            unsigned argc = o[0], ts = o[1];
            uint16_t c = rd16(o + 2), addr = rd16(o + 4);
            uint32_t thisptr = pop32();
            if (!have(argc)) break;
            std::vector<uint8_t> args(st.begin() + p.sp, st.begin() + p.sp + argc);
            std::vector<uint8_t> tb;
            bool has_this = ts > 0 && thisptr != 0;
            if (has_this) {
                tb.resize(ts);
                if (!read_ptr(thisptr, tb.data(), ts)) { error(p, "spawn: bad this pointer"); return; }
            }
            p.ip = next;  // the child may look at us
            auto child = std::make_unique<UCProcess>(*this, c, addr, has_this, std::move(tb), std::move(args));
            p.temp = kernel_.add_and_run(std::move(child));
            break;
        }
        case 0x58: {  // spawn.inline class func delta thissize unk
            uint16_t c = rd16(o), func = rd16(o + 2), delta = rd16(o + 4);
            unsigned ts = o[6];
            std::vector<uint8_t> tb;
            bool has_this = false;
            if (ts > 0) {
                uint8_t* q = at(6, 4);
                uint32_t thisptr = q ? rd32(q) : 0;
                if (thisptr) {
                    tb.resize(ts);
                    has_this = read_ptr(thisptr, tb.data(), ts);
                }
            }
            p.ip = next;
            auto child = std::make_unique<UCProcess>(*this, c, uint16_t(func + delta), has_this, std::move(tb),
                                                     std::vector<uint8_t>{});
            push16(kernel_.add_and_run(std::move(child)));
            break;
        }
        case 0x77: { uint16_t item = pop16(); uint16_t type = pop16(); p.item = item; p.type = type; break; }
        case 0x78:
            if (kernel_.count_exact(p.item, p.type) > 1) { p.terminate_deferred(); stop = true; }
            break;

        // --- loops
        case 0x74: if (need(1)) { p.sp -= 1; st[p.sp] = o[0]; } break;
        case 0x70: {  // loop var len type
            int var = int8_t(o[0]);
            unsigned slen = o[1], type = o[2];
            uint16_t a = pop16(), b = pop16();
            unsigned area = loop_area(type);
            if (!area || slen > 0x20) { error(p, "loop: bad search type or script"); return; }
            if (!have(slen)) break;
            uint32_t s_end = p.sp + slen;
            std::vector<uint16_t> ids;
            if (host_) host_->loop_search(int(type), a, b, &st[p.sp], slen, ids);
            if (s_end < area) { bad = true; break; }
            p.sp = s_end - area;
            uint16_t lid = new_list(2);
            UCList* l = list(lid);
            for (uint16_t id : ids) l->push_word(id);
            st[p.sp] = uint8_t(lid); st[p.sp + 1] = uint8_t(lid >> 8);
            st[p.sp + 2] = 0; st[p.sp + 3] = 0;
            st[p.sp + 4] = uint8_t(var); st[p.sp + 5] = 0;
        }
            [[fallthrough]];
        case 0x73: {  // loopnext
            if (!have(6)) break;
            uint16_t lid = uint16_t(st[p.sp] | (st[p.sp + 1] << 8));
            uint16_t pos = uint16_t(st[p.sp + 2] | (st[p.sp + 3] << 8));
            int var = int8_t(st[p.sp + 4]);
            UCList* l = list(lid);
            if (!l && lid) { error(p, "loopnext: snapshot list missing"); return; }
            bool found = false;
            while (l && pos < l->size()) {
                uint16_t id = l->word(pos++);
                if (host_ && !host_->object_exists(id)) continue;
                wrw(var, id);
                found = true;
                break;
            }
            if (found) {
                st[p.sp + 2] = uint8_t(pos); st[p.sp + 3] = uint8_t(pos >> 8);
                push16(1);
            } else {
                free_list(lid);
                st[p.sp] = 0; st[p.sp + 1] = 0;
                push16(0);
            }
            break;
        }
        case 0x75: case 0x76: {  // foreach var size rel
            int var = int8_t(o[0]);
            unsigned sz = o[1];
            if (!have(4)) break;
            uint16_t idx = uint16_t(st[p.sp] | (st[p.sp + 1] << 8));
            uint16_t lid = uint16_t(st[p.sp + 2] | (st[p.sp + 3] << 8));
            UCList* l = list(lid);
            if (!l) {  // vm.md: an error in the reference; treat as an empty list
                PG_LOG("usecode: foreach over missing list %u (class %04X:%04X)", lid, p.cls, p.ip);
                p.sp += 4;
                jump_to = uint16_t(next + rds16(o + 2));
                break;
            }
            idx = idx == 0xFFFF ? 0 : uint16_t(idx + 1);
            if (idx >= l->size()) {
                if (op == 0x76) free_slist(lid);
                else free_list(lid);
                p.sp += 4;
                jump_to = uint16_t(next + rds16(o + 2));
            } else {
                st[p.sp] = uint8_t(idx); st[p.sp + 1] = uint8_t(idx >> 8);
                if (uint8_t* q = at(var, sz)) {
                    if (l->elem == sz) std::memcpy(q, &l->data[idx * sz], sz);
                }
            }
            break;
        }
        default:
            error(p, "unhandled opcode");
            return;
        }

        if (bad) { error(p, "stack overflow/underflow or bad stack address"); return; }
        if (p.terminated()) return;  // killed by an intrinsic
        if (op != 0x50 || !stop) p.ip = jump_to;
        if (stop) {
            if (op == 0x53) p.ip = next;
            return;
        }
    }
}

}  // namespace pg
