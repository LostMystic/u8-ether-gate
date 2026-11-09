// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <functional>
#include <list>
#include <string>
#include <memory>
#include <vector>

#include "kernel/id_alloc.h"
#include "core/serial.h"

namespace pg {

class Kernel;

// Process type numbers that usecode can see (specs/world/kernel.md section 4).
enum ProcType : uint16_t {
    kProcAny = 6, kProcPersistent = 1, kProcActorAnim = 0xF0, kProcCombat = 0xF2, kProcGumpNotify = 0x200,
    kProcGravity = 0x203, kProcPathfind = 0x204, kProcLoiter = 0x205, kProcFireball = 0x218,
    kProcGrantPeace = 0x21D, kProcAmbush = 0x21E, kProcHeal = 0x222, kProcResurrect = 0x229,
    kProcDestroyItem = 0x232, kProcClearFeign = 0x243, kProcScheduler = 0x245,
};

// Declares a process class's savegame hooks (defined in game/savegame.cpp).
#define PG_SAVED_PROCESS(TAG)                                    \
    const char* save_tag() const override { return TAG; }       \
    void save(SaveWriter& out) const override;                  \
    void load(SaveReader& in) override;

// A cooperative task in the kernel run list.
class Process {
public:
    virtual ~Process() = default;
    // One turn. Called only when the process may run (not suspended or ending).
    virtual void run() = 0;
    // Called when the process ends (normally or failed), before waiters are woken.
    virtual void on_terminate() {}
    // Called once the pid is assigned, before the first turn.
    virtual void on_added() {}
    virtual const char* name() const { return "process"; }
    // Savegames: a tag naming the class (nullptr = not saved), and the class's own state.
    virtual const char* save_tag() const { return nullptr; }
    virtual void save(SaveWriter& out) const {}
    virtual void load(SaveReader& in) {}
    void save_base(SaveWriter& out) const;
    void load_base(SaveReader& in);

    uint16_t pid() const { return pid_; }
    uint16_t item = 0;
    uint16_t type = 0;
    uint32_t result = 0;
    bool run_while_paused = false;
    bool prevents_saving = false;
    unsigned ticks_per_run = 2;

    bool suspended() const { return suspended_; }
    bool terminated() const { return terminated_; }
    bool deferred() const { return deferred_; }
    bool failed() const { return failed_; }
    const std::vector<uint16_t>& waiters() const { return waiters_; }  // pids woken when it ends
    // Live = will still run or wake waiters later (not ending, not gone).
    bool live() const { return !terminated_ && !deferred_; }

    // Ends at the start of its next turn (waiters wake then).
    void terminate_deferred() { if (!terminated_) deferred_ = true; }
    // Ends now: waiters are woken and moved after the running process.
    void terminate();
    void fail() { failed_ = true; terminate(); }
    // Makes this process wait for `pid` (kernel.md 2): no-op if pid is finished; pid 0
    // suspends with nobody to wake it.
    void wait_for(uint16_t pid);
    void suspend() { suspended_ = true; }
    void wake(uint32_t res);

    Kernel* kernel() const { return kernel_; }
    // Turn-based combat (game/turn_combat.h): a number that grows with every process added
    // (also on load), so "created after a moment" can be told apart even when pids are reused.
    uint64_t serial() const { return serial_; }

private:
    friend class Kernel;
    Kernel* kernel_ = nullptr;
    uint64_t serial_ = 0;
    uint16_t pid_ = 0;
    bool suspended_ = false, terminated_ = false, deferred_ = false, failed_ = false;
    std::vector<uint16_t> waiters_;
    std::list<Process*>::iterator self_;
};

// The run list and the 60 Hz tick (specs/world/kernel.md sections 1-5).
class Kernel {
public:
    Kernel();
    ~Kernel();
    void reset();

    // Normal add: after the running process during a pass, else at the front. Returns the pid
    // (0 if the pid space is exhausted; the process is then deleted).
    uint16_t add(std::unique_ptr<Process> p);
    // Add and run now: appended at the end and given one nested turn immediately.
    uint16_t add_and_run(std::unique_ptr<Process> p);

    // One pass (one tick).
    void pass();
    // One rendered frame: two passes.
    void frame() { pass(); pass(); }

    Process* get(uint16_t pid) const { return pid < table_.size() ? table_[pid] : nullptr; }
    Process* running() const { return running_; }
    uint32_t tick() const { return tick_; }
    uint32_t frame_number() const { return tick_ / 2; }
    void set_tick(uint32_t t) { tick_ = t; }

    void pause() { ++pause_; }
    void unpause() { if (pause_) --pause_; }
    bool paused() const { return pause_ > 0; }

    // Kill rules (kernel.md 2): item 0 = any, type 6 = any; processes with item 0 are never
    // matched. Matching processes are failed. Returns how many.
    unsigned kill(uint16_t item, uint16_t type);
    unsigned kill_all_except_type(uint16_t item, uint16_t keep_type);
    // Live processes of (item, type); item 0 = any, type 6 = any.
    unsigned count(uint16_t item, uint16_t type) const;
    // Live processes with exactly this (item, type).
    unsigned count_exact(uint16_t item, uint16_t type) const;
    // The first live process of (item, type), or nullptr.
    Process* find(uint16_t item, uint16_t type) const;
    // Map change: fail everything except item 0 and type 1.
    void kill_for_map_change();
    size_t size() const { return list_.size(); }
    template <class F> void for_each(F f) const { for (Process* p : list_) f(p); }
    uint64_t runs() const { return runs_; }
    // Turn-based combat (game/turn_combat.h): the serial of the newest process, and a filter
    // that holds a process for this pass (it keeps its place and state, it just does not run).
    uint64_t serial() const { return serial_; }
    std::function<bool(const Process&)> hold;

    // Savegames: every live process in run-list order, with its pid and waiters. `make`
    // builds an empty process of a tag's class; false if a tag is unknown or the data is bad.
    void save(SaveWriter& out) const;
    bool load(SaveReader& in, const std::function<std::unique_ptr<Process>(const std::string&)>& make);

private:
    friend class Process;
    void move_after_current(Process* p);
    void remove(Process* p);

    std::list<Process*> list_;
    std::vector<Process*> table_;
    IdAllocator pids_{1, 32766};
    std::list<Process*>::iterator current_;
    bool in_pass_ = false;
    Process* running_ = nullptr;
    uint32_t tick_ = 0;
    unsigned pause_ = 0;
    uint64_t runs_ = 0;
    uint64_t serial_ = 0;
};

// Counts down N runs, then ends (type 0).
class DelayProcess : public Process {
public:
    explicit DelayProcess(int frames) : left_(frames) {}
    void run() override { if (--left_ <= 0) terminate(); }
    const char* name() const override { return "delay"; }
    const char* save_tag() const override { return "delay"; }
    void save(SaveWriter& out) const override { out.i32(left_); }
    void load(SaveReader& in) override { left_ = in.i32(); }

private:
    int left_;
};

}  // namespace pg
