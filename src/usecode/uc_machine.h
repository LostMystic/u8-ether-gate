// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "kernel/kernel.h"

namespace pg {

class UsecodeFile;
class UCMachine;

// Usecode pointers: segment << 16 | offset (specs/usecode/vm.md section 3).
enum : uint16_t { kSegString = 0x8000, kSegObject = 0x8002, kSegGlobal = 0x8003 };
inline uint32_t make_ptr(uint16_t seg, uint16_t off) { return (uint32_t(seg) << 16) | off; }

// A running usecode script (vm.md section 2).
class UCProcess : public Process {
public:
    static constexpr unsigned kStackSize = 0x1000;

    // Starts function (cls, addr). If has_this, `this_bytes` (the item id word) is pushed first
    // and a this pointer to it is pushed after the argument block (vm.md 4.4, 5.3).
    UCProcess(UCMachine& m, uint16_t cls, uint16_t addr, bool has_this, std::vector<uint8_t> this_bytes,
              std::vector<uint8_t> args);
    void run() override;
    void on_added() override;
    void on_terminate() override;
    const char* name() const override { return "usecode"; }
    const char* save_tag() const override { return "usecode"; }
    void save(SaveWriter& out) const override;
    void load(SaveReader& in) override;

    uint16_t cls = 0, ip = 0, bp = 0;
    uint32_t sp = kStackSize;
    uint32_t temp = 0;
    std::array<uint8_t, kStackSize> stack{};
    std::vector<std::pair<uint16_t, int>> free_on_exit;  // (id, kind 1 string / 2 slist / 3 list)
    uint16_t start_cls = 0, start_addr = 0;

private:
    bool has_this_;
    std::vector<uint8_t> this_bytes_, args_;
    UCMachine& m_;
};

struct UCList {
    unsigned elem = 2;
    std::vector<uint8_t> data;
    unsigned size() const { return elem ? unsigned(data.size() / elem) : 0; }
    uint16_t word(unsigned i) const { return uint16_t(data[i * elem] | (data[i * elem + 1] << 8)); }
    void push_word(uint16_t v) { data.push_back(uint8_t(v)); data.push_back(uint8_t(v >> 8)); }
};

// The world side the VM needs for search loops (specs/usecode/loops.md).
class UsecodeHost {
public:
    virtual ~UsecodeHost() = default;
    // Fills `out` with matching object ids for loop type 2..6.
    virtual void loop_search(int type, uint16_t a, uint16_t b, const uint8_t* script, unsigned len,
                             std::vector<uint16_t>& out) = 0;
    virtual bool object_exists(uint16_t id) const = 0;
};

// An intrinsic call: the argument block, first parameter at offset 0 (intrinsics/index.md).
struct IntrinsicCall {
    UCMachine& m;
    UCProcess& proc;
    const uint8_t* args;
    unsigned size;
    uint16_t number;

    uint16_t u16(unsigned off) const { return off + 2 <= size ? uint16_t(args[off] | (args[off + 1] << 8)) : 0; }
    int16_t s16(unsigned off) const { return int16_t(u16(off)); }
    uint32_t u32(unsigned off) const { return uint32_t(u16(off)) | (uint32_t(u16(off + 2)) << 16); }
    // A 4-byte pointer argument resolved to the 16-bit value it names (object id, string id).
    uint16_t obj(unsigned off) const;
    std::string str(unsigned off) const;  // string pointer argument
    bool write16(unsigned ptr_off, uint16_t v) const;
    bool write_bytes(unsigned ptr_off, const uint8_t* b, unsigned n) const;
};
using IntrinsicFn = std::function<uint32_t(IntrinsicCall&)>;

// Usecode machine: globals, string and list heaps, intrinsics, the interpreter.
class UCMachine {
public:
    static constexpr unsigned kIntrinsics = 0x102;

    UCMachine(const UsecodeFile& file, Kernel& kernel);
    void reset();  // new game: heaps and globals cleared
    void set_host(UsecodeHost* h) { host_ = h; }

    const UsecodeFile& file() const { return file_; }
    Kernel& kernel() { return kernel_; }

    // Starts function (cls, addr) as a new process. has_this: `this_id` is the item.
    // run_now: "add and run now" (spawn), else a normal add (events). Returns the pid.
    uint16_t start(uint16_t cls, uint16_t addr, bool has_this, uint16_t this_id, const std::vector<uint8_t>& args,
                   bool run_now);
    // Starts event `e` of class `cls` with item `item`. 0 if there is no handler.
    uint16_t start_event(uint16_t cls, unsigned e, uint16_t item, const std::vector<uint8_t>& args = {});

    // Intrinsics.
    void set_intrinsic(unsigned n, IntrinsicFn fn, const char* name = nullptr);
    bool has_intrinsic(unsigned n) const { return n < kIntrinsics && bool(intr_[n]); }
    const std::set<unsigned>& unimplemented_hit() const { return unimpl_hit_; }
    const std::vector<uint64_t>& intrinsic_calls() const { return intr_calls_; }
    unsigned implemented_count() const;

    // Globals (vm.md 7).
    uint16_t get_global(unsigned pos, unsigned n) const;
    void set_global(unsigned pos, unsigned n, uint16_t v);

    // Strings (vm.md 8). Id 0 reads as "".
    uint16_t new_string(const std::string& s);
    const std::string& string(uint16_t id) const;
    bool has_string(uint16_t id) const { return strings_.count(id) != 0; }
    void free_string(uint16_t id);
    uint16_t dup_string(uint16_t id) { return new_string(string(id)); }
    size_t string_count() const { return strings_.size(); }

    // Lists.
    uint16_t new_list(unsigned elem = 2);
    UCList* list(uint16_t id);
    void free_list(uint16_t id);
    void free_slist(uint16_t id);  // and its strings
    uint16_t dup_list(uint16_t id, unsigned elem);
    uint16_t dup_slist(uint16_t id);
    size_t list_count() const { return lists_.size(); }

    // Pointers.
    bool read_ptr(uint32_t ptr, uint8_t* out, unsigned n) const;
    bool write_ptr(uint32_t ptr, const uint8_t* in, unsigned n);
    // Resolves a pointer to the 16-bit value it names (object or string id); 0 on failure.
    uint16_t resolve16(uint32_t ptr) const;

    // Interpreter, used by UCProcess::run.
    void execute(UCProcess& p);
    uint64_t instructions() const { return instructions_; }
    // Savegames: globals, string and list heaps (vm.md 7-8).
    void save(SaveWriter& out) const;
    bool load(SaveReader& in);
    unsigned errors() const { return errors_; }
    bool trace = false;
    bool trace_intrinsics = false;  // log every intrinsic call (debugging)
    // Debugging / tests: called for every global read (write false) or write by a script (opcodes 0x4E / 0x4F).
    std::function<void(uint16_t cls, unsigned pos, unsigned n, bool write, uint16_t v)> global_watch;
    // Called for every string a script creates (push.str), after the story patches' corrections,
    // with the class whose code runs; may change the text (game/avatar_title.h: "everyone knows").
    std::function<void(uint16_t cls, std::string& text)> text_hook;

private:
    bool error(UCProcess& p, const char* what);

    const UsecodeFile& file_;
    Kernel& kernel_;
    UsecodeHost* host_ = nullptr;
    std::array<uint8_t, 512> globals_{};
    std::unordered_map<uint16_t, std::string> strings_;
    std::unordered_map<uint16_t, UCList> lists_;
    IdAllocator string_ids_{1, 65534}, list_ids_{1, 65534};
    std::vector<IntrinsicFn> intr_;
    std::vector<std::string> intr_names_;
    std::vector<uint64_t> intr_calls_;
    std::set<unsigned> unimpl_hit_;
    uint64_t instructions_ = 0;
    unsigned errors_ = 0;
};

// Length in bytes of the instruction at p (0 if the opcode is unknown or truncated).
unsigned uc_op_length(const uint8_t* p, size_t remain);

}  // namespace pg
