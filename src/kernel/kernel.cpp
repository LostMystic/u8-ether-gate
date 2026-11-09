// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#include "kernel/kernel.h"

#include <algorithm>

#include "core/log.h"

namespace pg {

void Process::terminate() {
    if (terminated_) return;
    terminated_ = true;
    deferred_ = false;
    on_terminate();
    if (!kernel_) return;
    std::vector<uint16_t> w;
    w.swap(waiters_);
    for (uint16_t pid : w)
        if (Process* p = kernel_->get(pid)) p->wake(result);
}

void Process::wake(uint32_t res) {
    result = res;
    suspended_ = false;
    if (kernel_ && !terminated_) kernel_->move_after_current(this);
}

void Process::wait_for(uint16_t pid) {
    if (pid == 0) { suspended_ = true; return; }
    Process* other = kernel_ ? kernel_->get(pid) : nullptr;
    if (!other || other->terminated_) return;  // finished already: no-op
    if (other == this) return;
    other->waiters_.push_back(pid_);
    suspended_ = true;
}

Kernel::Kernel() : table_(32768, nullptr) {}

Kernel::~Kernel() { reset(); }

void Kernel::reset() {
    for (Process* p : list_) delete p;
    list_.clear();
    std::fill(table_.begin(), table_.end(), nullptr);
    pids_.reset(1, 32766);
    in_pass_ = false;
    running_ = nullptr;
    tick_ = 0;
    pause_ = 0;
}

uint16_t Kernel::add(std::unique_ptr<Process> up) {
    uint16_t pid = pids_.allocate();
    if (!pid) { PG_LOG("kernel: out of pids"); return 0; }
    Process* p = up.release();
    p->kernel_ = this;
    p->pid_ = pid;
    p->serial_ = ++serial_;
    table_[pid] = p;
    if (in_pass_) p->self_ = list_.insert(std::next(current_), p);
    else p->self_ = list_.insert(list_.begin(), p);
    p->on_added();
    return pid;
}

uint16_t Kernel::add_and_run(std::unique_ptr<Process> up) {
    uint16_t pid = pids_.allocate();
    if (!pid) { PG_LOG("kernel: out of pids"); return 0; }
    Process* p = up.release();
    p->kernel_ = this;
    p->pid_ = pid;
    p->serial_ = ++serial_;
    table_[pid] = p;
    p->self_ = list_.insert(list_.end(), p);
    p->on_added();
    Process* outer = running_;
    running_ = p;
    if (p->live() && !p->suspended()) { p->run(); ++runs_; }
    running_ = outer;
    return pid;
}

void Kernel::move_after_current(Process* p) {
    if (!in_pass_) return;  // keeps its place
    if (p->self_ == current_) return;
    list_.splice(std::next(current_), list_, p->self_);
}

void Kernel::remove(Process* p) {
    table_[p->pid_] = nullptr;
    pids_.release(p->pid_);
    delete p;
}

void Kernel::pass() {
    bool paused_now = paused();
    if (!paused_now) ++tick_;
    in_pass_ = true;
    unsigned runs_this_pass = 0;
    for (current_ = list_.begin(); current_ != list_.end();) {
        Process* p = *current_;
        if (!paused_now && p->deferred_ && !p->terminated_) p->terminate();
        bool may = !p->terminated_ && !p->deferred_ && !p->suspended_ && (!paused_now || p->run_while_paused) &&
                   (paused_now || tick_ % p->ticks_per_run == 0);
        if (may && hold && hold(*p)) may = false;  // turn-based combat: frozen this pass
        if (may) {
            running_ = p;
            p->run();
            running_ = nullptr;
            ++runs_;
            if (++runs_this_pass > 65534) {
                PG_LOG("kernel: too many runs in one pass, failing pid %u", p->pid_);
                p->fail();
            }
        }
        if (!paused_now && p->terminated_) {
            current_ = list_.erase(current_);
            remove(p);
        } else {
            ++current_;
        }
    }
    in_pass_ = false;
}

static bool matches(const Process* p, uint16_t item, uint16_t type) {
    if (p->item == 0) return false;
    if (item != 0 && p->item != item) return false;
    if (type != kProcAny && p->type != type) return false;
    return true;
}

unsigned Kernel::kill(uint16_t item, uint16_t type) {
    std::vector<Process*> hit;
    for (Process* p : list_)
        if (p->live() && matches(p, item, type)) hit.push_back(p);
    for (Process* p : hit) p->fail();
    return unsigned(hit.size());
}

unsigned Kernel::kill_all_except_type(uint16_t item, uint16_t keep_type) {
    std::vector<Process*> hit;
    for (Process* p : list_)
        if (p->live() && p->item == item && item != 0 && p->type != keep_type) hit.push_back(p);
    for (Process* p : hit) p->fail();
    return unsigned(hit.size());
}

unsigned Kernel::count(uint16_t item, uint16_t type) const {
    unsigned n = 0;
    for (Process* p : list_)
        if (p->live() && (item == 0 || p->item == item) && (type == kProcAny || p->type == type)) ++n;
    return n;
}

unsigned Kernel::count_exact(uint16_t item, uint16_t type) const {
    unsigned n = 0;
    for (Process* p : list_) n += (p->live() && p->item == item && p->type == type) ? 1 : 0;
    return n;
}

Process* Kernel::find(uint16_t item, uint16_t type) const {
    for (Process* p : list_)
        if (p->live() && (item == 0 || p->item == item) && (type == kProcAny || p->type == type)) return p;
    return nullptr;
}

void Kernel::kill_for_map_change() {
    std::vector<Process*> hit;
    for (Process* p : list_)
        if (p->live() && p->item != 0 && p->type != kProcPersistent) hit.push_back(p);
    for (Process* p : hit) p->fail();
}

// ---------------------------------------------------------------------------------------------
// Savegames

void Process::save_base(SaveWriter& out) const {
    out.u16(pid_);
    out.u16(item);
    out.u16(type);
    out.u32(result);
    out.b(run_while_paused);
    out.b(prevents_saving);
    out.u32(ticks_per_run);
    out.b(suspended_);
    out.b(deferred_);
    out.u32(uint32_t(waiters_.size()));
    for (uint16_t w : waiters_) out.u16(w);
}

void Process::load_base(SaveReader& in) {
    pid_ = uint16_t(in.u16());
    item = uint16_t(in.u16());
    type = uint16_t(in.u16());
    result = in.u32();
    run_while_paused = in.b();
    prevents_saving = in.b();
    ticks_per_run = std::max(1u, in.u32());
    suspended_ = in.b();
    deferred_ = in.b();
    waiters_.clear();
    unsigned n = in.u32();
    for (unsigned i = 0; i < n && !in.fail(); ++i) waiters_.push_back(uint16_t(in.u16()));
}

void Kernel::save(SaveWriter& out) const {
    out.tag("KERN");
    out.u32(tick_);
    out.u64(runs_);
    pids_.save(out);
    std::vector<const Process*> keep;
    for (const Process* p : list_)
        if (!p->terminated_ && p->save_tag()) keep.push_back(p);
        else if (!p->terminated_) PG_LOG("save: process %u (%s) is not saved", p->pid_, p->name());
    out.u32(uint32_t(keep.size()));
    for (const Process* p : keep) {
        out.str(p->save_tag());
        p->save_base(out);
        p->save(out);
    }
}

bool Kernel::load(SaveReader& in, const std::function<std::unique_ptr<Process>(const std::string&)>& make) {
    reset();
    if (!in.tag("KERN")) return false;
    tick_ = in.u32();
    runs_ = in.u64();
    pids_.load(in);
    unsigned n = in.u32();
    for (unsigned i = 0; i < n && !in.fail(); ++i) {
        std::string tag = in.str();
        std::unique_ptr<Process> up = make(tag);
        if (!up) { PG_LOG("load: unknown process class '%s'", tag.c_str()); in.set_fail(); return false; }
        up->load_base(in);
        up->load(in);
        if (in.fail()) return false;
        Process* p = up.release();
        if (p->pid_ == 0 || p->pid_ >= table_.size() || table_[p->pid_]) { delete p; in.set_fail(); return false; }
        p->kernel_ = this;
        table_[p->pid_] = p;
        p->serial_ = ++serial_;
        p->self_ = list_.insert(list_.end(), p);
        pids_.reserve(p->pid_);
    }
    // Pids held by processes that were not saved (already ended) are free again.
    for (unsigned id = pids_.first(); id <= pids_.last(); ++id)
        if (pids_.used(id) && !table_[id]) pids_.release(id);
    // Waiters that no longer exist are dropped.
    for (Process* p : list_) {
        std::vector<uint16_t> keep;
        for (uint16_t w : p->waiters_)
            if (get(w)) keep.push_back(w);
        p->waiters_.swap(keep);
    }
    return !in.fail();
}

}  // namespace pg
