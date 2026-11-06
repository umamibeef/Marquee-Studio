#include <fstream>
#include "machine.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace sim {

using clk = std::chrono::steady_clock;

constexpr uint32_t THREAD_EXECUTING = 0x45C4CC;         // _Thread_Executing
constexpr uint32_t CONTEXT = 0xE0;                      // a TCB's saved CPU context

Machine::Machine(const Repo& r, MachineConfig cfg) : repo(r), cfg_(std::move(cfg)) {
    // Our firmware (the app entry of firmware/app/mkimg.py) touches the hardware itself, so it runs with none of the
    // hooks at the original's addresses, on the sign's 200 x 16 geometry, and has the DB9 on SCC1. Known before the
    // thread starts: the DB9's device is attached as soon as this returns.
    {
        auto img = read_file(cfg_.flash);
        static const char NAME[] = "UMAMI_APP";
        from_source_ = cfg_.app.empty() && std::search(img.begin(), img.end(), NAME, NAME + sizeof NAME - 1) != img.end();
    }
    thread_ = std::thread([this] { loop(); });
}

Machine::~Machine() { quit(); }

void Machine::quit() {
    if (quit_.exchange(true))
        return;
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();                                  // the emulation thread closes the Board itself
}

void Machine::post(std::function<void(Board&)> fn) {
    {
        std::lock_guard<std::mutex> g(mu_);
        queue_.push_back(std::move(fn));
    }
    cv_.notify_all();
}

void Machine::boot() {
    // A reset (the toolbar's, or the firmware's own restart) is a power cycle: the main flash (the content store, the
    // update slot) and the NVRAM keep what they hold, as on the real board.
    std::vector<uint8_t> keep_flash, keep_nvram;
    if (b_) {
        keep_flash = b_->main_flash;
        keep_nvram.resize(0x8000);
        if (!b_->read(NVRAM, keep_nvram.data(), keep_nvram.size()))
            keep_nvram.clear();
    }
    // (the old board stays until the new one has booted: a boot that fails leaves the machine as it was)
    Scenario sc;
    sc.geom_word = cfg_.geom_word, sc.geom_w = cfg_.geom_w, sc.geom_h = cfg_.geom_h;
    if (from_source_)
        sc.geom_word = 0xC240, sc.geom_w = 200, sc.geom_h = 16;
    cfg_.geom_w = sc.geom_w, cfg_.geom_h = sc.geom_h;
    if (sc.geom_w == 200 && sc.geom_h == 16)             // ED0501/AD0501: panel_width/height = 240x128 (EEPROM 0x13)
        cfg_.panel_w = 240, cfg_.panel_h = 128;
    auto b = std::make_unique<Board>(read_file(cfg_.flash), sc,
                                     cfg_.app.empty() ? std::vector<uint8_t>() : read_file(cfg_.app));
    if (!keep_flash.empty())
        b->main_flash = std::move(keep_flash);
    if (!keep_nvram.empty())
        b->write(NVRAM, keep_nvram.data(), keep_nvram.size());
    b->frames_kept = 4;
    if (from_source_) {
        b->fpga_model = true;
        auto a0 = repo.path("dumps/serial/eeprom_a0.bin"), a2 = repo.path("dumps/serial/eeprom_a2.bin");   // the sign's, when here
        if (std::ifstream(a0).good() && std::ifstream(a2).good())
            b->eeproms[0xA0] = read_file(a0), b->eeproms[0xA2] = read_file(a2);
    }
    if (!b->boot())
        throw std::runtime_error("the boot ROM did not reach the app");
    if (cfg_.app.empty()) {                              // the app the boot ROM unpacked: its code is what gets hooked
        b->app.resize(CODE_END);
        b->read(0, b->app.data(), CODE_END);
    } else
        b->load_app_image();
    b->hook_app_specials();
    if (!from_source_) {
        b->hook_busy_waits(repo.symbols);
        b->watch_switches(repo.symbols);
    }
    if (cfg_.cougar) {
        b->force_config_word(0xDCDE4, 8);
        b->force_config_bytes(0xDCDF0, {0x01, 0x0F, 0x0F, 0x0F});
    }
    b->in_app = true;
    b_ = std::move(b);
    watch_hooks_.clear();
    rehook_watchpoints();
}

void Machine::stopped(const std::string& why, int sig) {
    running_ = false;
    b_->tr(Board::TR_STOP);
    {
        std::lock_guard<std::mutex> g(view_mu_);
        view_.stop_reason = why;
    }
    refresh(true);
    if (on_stop) {
        uint32_t tid = 1;
        for (auto& t : tasks_now())
            if (t.current)
                tid = t.id;
        on_stop(b_->pc(), sig, tid);
    }
}

void Machine::loop() {
    try {
        boot();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> g(view_mu_);
        view_.stop_reason = std::string("boot failed: ") + e.what();
        return;
    }
    refresh(true);
    auto last_view = clk::now();
    auto wall0 = clk::now();
    uint64_t sim0 = b_->now();
    uint64_t count0 = b_->exact_count();
    auto rate_t = clk::now();
    while (!quit_) {
        std::deque<std::function<void(Board&)>> work;
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (queue_.empty() && !running_)
                cv_.wait_for(lk, std::chrono::milliseconds(50));
            work.swap(queue_);
        }
        for (auto& fn : work)
            fn(*b_);
        if (!running_) {
            wall0 = clk::now();
            sim0 = b_->now();
            if (clk::now() - last_view > std::chrono::milliseconds(100)) {
                refresh(false);
                last_view = clk::now();
            }
            continue;
        }
        // real time: the sign's clock (CPU_MIPS instructions per microsecond) never runs ahead of the wall clock
        if (realtime) {
            double sim_s = double(b_->now() - sim0) / (CPU_MIPS * 1e6);
            double wall_s = std::chrono::duration<double>(clk::now() - wall0).count();
            if (sim_s > wall_s + 0.002) {
                std::this_thread::sleep_for(std::chrono::duration<double>(std::min(sim_s - wall_s, 0.02)));
                continue;
            }
            if (wall_s > sim_s + 0.25)                    // cannot keep up: do not try to catch up later
                wall0 = clk::now(), sim0 = b_->now();
        } else
            wall0 = clk::now(), sim0 = b_->now();
        watch_hit_ = false;
        b_->advance(UINT64_MAX);
        if (b_->reset_requested) {                        // the firmware reset the CPU (PLPRCR CSR): the board restarts
            fprintf(stderr, "the firmware reset the CPU: restarting the board (main flash and NVRAM kept)\n");
            auto bps = b_->breakpoints;
            if (!reboot("the firmware's restart"))
                continue;
            fprintf(stderr, "the board restarted: the boot reached the firmware\n");
            b_->breakpoints = bps;
            wall0 = clk::now(), sim0 = b_->now();
            refresh(true);
            continue;
        }
        if (watch_hit_) {
            {
                std::lock_guard<std::mutex> g(view_mu_);
                view_.watch_addr = watch_addr_;
                view_.watch_kind = 1;
                for (auto& w : watch_)
                    if (watch_addr_ >= w.addr && watch_addr_ < w.addr + w.len)
                        view_.watch_kind = w.kind;
            }
            stopped("watchpoint", 5);
            continue;
        }
        if (b_->hit_breakpoint) {
            bool temp = b_->hit_pc == temp_bp_;
            if (temp_bp_ != UINT64_MAX) {
                b_->breakpoints.erase((uint32_t)temp_bp_);
                temp_bp_ = UINT64_MAX;
            }
            stopped(temp ? "step over" : "breakpoint", 5);
            continue;
        }
        if (clk::now() - last_view > std::chrono::milliseconds(16)) {
            double dt = std::chrono::duration<double>(clk::now() - rate_t).count();
            if (dt > 0.25) {
                std::lock_guard<std::mutex> g(view_mu_);
                view_.mips = double(b_->exact_count() - count0) / dt / 1e6;
                count0 = b_->exact_count();
                rate_t = clk::now();
            }
            refresh(clk::now() - last_view > std::chrono::milliseconds(200));
            last_view = clk::now();
        }
    }
    b_.reset();
}

std::vector<Task> Machine::tasks_now() {
    Board& b = *b_;
    uint32_t ex = b.r32(THREAD_EXECUTING);
    auto tcbs = b.tcbs;
    bool known = false;
    for (auto& [n, t] : tcbs)
        known |= t == ex;
    if (ex && !known)
        tcbs[task_name(b, ex)] = ex;
    std::vector<Task> out;
    for (auto& [name, tcb] : tcbs) {
        Task t;
        t.tcb = tcb, t.name = name, t.id = b.r32(tcb + 8), t.state = b.r32(tcb + 0x10), t.prio = b.r32(tcb + 0x14);
        t.current = tcb == ex;
        if (t.current) {
            for (int i = 0; i < 32; i++)
                t.regs[i] = b.reg(UC_PPC_REG_0 + i);
            t.regs[32] = b.pc(), t.regs[33] = (uint32_t)b.msr, t.regs[34] = b.reg(UC_PPC_REG_CR);
            t.regs[35] = b.reg(UC_PPC_REG_LR), t.regs[36] = b.reg(UC_PPC_REG_CTR), t.regs[37] = b.reg(UC_PPC_REG_XER);
        } else {
            uint32_t ctx = tcb + CONTEXT;
            t.regs[1] = b.r32(ctx), t.regs[2] = b.r32(ctx + 4);
            for (int i = 13; i < 32; i++)
                t.regs[i] = b.r32(ctx + 8 + 4 * (i - 13));
            t.regs[34] = b.r32(ctx + 0x54), t.regs[35] = b.r32(ctx + 0x58), t.regs[33] = b.r32(ctx + 0x5C);
            t.regs[32] = t.regs[35];                     // it resumes at its saved LR
        }
        out.push_back(t);
    }
    std::sort(out.begin(), out.end(), [](const Task& a, const Task& c) { return a.id < c.id; });
    return out;
}

void Machine::refresh(bool full) {
    Board& b = *b_;
    View v;
    {
        std::lock_guard<std::mutex> g(view_mu_);
        v.mips = view_.mips;
        v.stop_reason = view_.stop_reason;
        v.watch_addr = view_.watch_addr;
        v.watch_kind = view_.watch_kind;
        if (!full) {
            v.tasks = view_.tasks;
            for (int i = 0; i < 6; i++)
                v.serial[i] = view_.serial[i];
        }
    }
    v.running = running_, v.booted = true;
    v.pc = b.pc();
    for (int i = 0; i < 32; i++)
        v.regs[i] = b.reg(UC_PPC_REG_0 + i);
    v.regs[32] = v.pc, v.regs[33] = (uint32_t)b.msr, v.regs[34] = b.reg(UC_PPC_REG_CR);
    v.regs[35] = b.reg(UC_PPC_REG_LR), v.regs[36] = b.reg(UC_PPC_REG_CTR), v.regs[37] = b.reg(UC_PPC_REG_XER);
    v.icount = b.exact_count(), v.frames = b.frame_total, v.now = b.now();
    if (!b.frames.empty())
        v.frame = b.frames.back();
    v.disp_w = cfg_.geom_w, v.disp_h = cfg_.geom_h;
    v.panel_w = cfg_.panel_w, v.panel_h = cfg_.panel_h;
    v.events = b.events;
    if (full) {
        v.tasks = tasks_now();
        for (auto& [m, data] : b.serial_tx)
            if (m >= 0 && m < 6)
                v.serial[m] = data;
    } else
        for (auto& [m, data] : b.serial_tx)
            if (m >= 0 && m < 6 && data.size() != v.serial[m].size())
                v.serial[m] = data;
    b.trace_on = tracing;
    std::lock_guard<std::mutex> g(view_mu_);
    view_ = std::move(v);
    if (!b.trace.empty()) {
        if (trace_.size() < (1u << 22))
            trace_.insert(trace_.end(), b.trace.begin(), b.trace.end());
        b.trace.clear();
        trace_names_ = b.trace_names;
    }
}

std::vector<Board::TraceEvent> Machine::take_trace(std::vector<std::string>* names) {
    std::lock_guard<std::mutex> g(view_mu_);
    std::vector<Board::TraceEvent> out;
    out.swap(trace_);
    if (names)
        *names = trace_names_;
    return out;
}

Machine::View Machine::view() {
    std::lock_guard<std::mutex> g(view_mu_);
    return view_;
}

void Machine::run() {
    post([this](Board&) {
        running_ = true;
        std::lock_guard<std::mutex> g(view_mu_);
        view_.stop_reason.clear();
    });
}

void Machine::halt() {
    post([this](Board&) {
        if (running_)
            stopped("halted", 2);
    });
}

void Machine::step(uint64_t n) {
    post([this, n](Board& b) {
        running_ = false;
        uint64_t left = n;
        while (left) {
            uint64_t ran = b.advance(left);
            left -= std::min(left, ran ? ran : 1);
            if (b.hit_breakpoint)
                break;
        }
        stopped("step", 5);
    });
}

void Machine::step_over() {
    post([this](Board& b) {
        uint32_t pc = b.pc(), w = b.r32(pc);
        uint32_t op = w >> 26;
        bool call = (op == 18 && (w & 1)) || (op == 16 && (w & 1)) || (op == 19 && (w & 1) &&
                                                                      (((w >> 1) & 0x3FF) == 16 || ((w >> 1) & 0x3FF) == 528));
        if (!call) {
            b.advance(1);
            stopped("step", 5);
            return;
        }
        if (!b.breakpoints.count(pc + 4)) {
            temp_bp_ = pc + 4;
            b.breakpoints.insert(pc + 4);
        }
        running_ = true;
    });
}

// boot() for a reset while the machine runs: a boot that fails stops the machine and says why, rather than taking
// the simulator down
bool Machine::reboot(const char* why) {
    try {
        boot();
        return true;
    } catch (const std::exception& e) {
        running_ = false;
        fprintf(stderr, "%s: the board did not boot (%s)\n", why, e.what());
        std::lock_guard<std::mutex> g(view_mu_);
        view_.stop_reason = std::string(why) + ": the board did not boot (" + e.what() + ")";
        return false;
    }
}

void Machine::reset() {
    post([this](Board&) {
        running_ = false;
        auto bps = b_->breakpoints;
        if (!reboot("reset"))
            return;
        b_->breakpoints = bps;
        std::lock_guard<std::mutex> g(view_mu_);
        view_.stop_reason = "reset";
    });
    post([this](Board&) { refresh(true); });
}

void Machine::set_geometry(uint16_t word, int w, int h) {
    post([this, word, w, h](Board&) {
        if (cfg_.geom_word == word && cfg_.geom_w == w && cfg_.geom_h == h)
            return;
        cfg_.geom_word = word, cfg_.geom_w = w, cfg_.geom_h = h;
        bool was_running = running_;
        running_ = false;
        auto bps = b_->breakpoints;
        boot();
        b_->breakpoints = bps;
        running_ = was_running;
        std::lock_guard<std::mutex> g(view_mu_);
        view_.stop_reason = "display model changed";
    });
    post([this](Board&) { refresh(true); });
}

bool Machine::read_mem(uint32_t a, uint8_t* out, size_t n) {
    return call([&](Board& b) {
        if (a >= IMMR && (uint64_t)a + n <= (uint64_t)IMMR + 0x10000) {
            memcpy(out, &b.dpram[a - IMMR], n);
            return true;
        }
        return b.read(a, out, n);
    });
}

bool Machine::write_mem(uint32_t a, const uint8_t* in, size_t n) {
    return call([&](Board& b) {
        bool ok = b.write(a, in, n);
        if (ok)
            uc_ctl_remove_cache(b.uc, a & ~3u, a + n + 4);   // code may have changed
        return ok;
    });
}

std::vector<uint32_t> Machine::regs() {
    return call([](Board& b) {
        std::vector<uint32_t> r(38);
        for (int i = 0; i < 32; i++)
            r[i] = b.reg(UC_PPC_REG_0 + i);
        r[32] = b.pc(), r[33] = (uint32_t)b.msr, r[34] = b.reg(UC_PPC_REG_CR);
        r[35] = b.reg(UC_PPC_REG_LR), r[36] = b.reg(UC_PPC_REG_CTR), r[37] = b.reg(UC_PPC_REG_XER);
        return r;
    });
}

void Machine::set_reg(int i, uint32_t v) {
    call([&](Board& b) {
        static const int ids[6] = {UC_PPC_REG_PC, 0, UC_PPC_REG_CR, UC_PPC_REG_LR, UC_PPC_REG_CTR, UC_PPC_REG_XER};
        if (i < 32)
            b.set_reg(UC_PPC_REG_0 + i, v);
        else if (i == 33)
            b.msr = v;                                   // the shadow MSR
        else if (i < 38)
            b.set_reg(ids[i - 32], v);
        refresh(true);
    });
}

std::vector<Task> Machine::tasks() {
    return call([this](Board&) { return tasks_now(); });
}

void Machine::set_breakpoint(uint32_t a, bool on) {
    call([&](Board& b) {
        if (on)
            b.breakpoints.insert(a);
        else
            b.breakpoints.erase(a);
        uc_ctl_remove_cache(b.uc, a, a + 4);             // blocks already translated across it are checked again
    });
}

std::vector<uint32_t> Machine::breakpoints() {
    return call([](Board& b) { return std::vector<uint32_t>(b.breakpoints.begin(), b.breakpoints.end()); });
}

void watch_cb(uc_engine* uc, uc_mem_type, uint64_t addr, int, int64_t, void* ud) {
    auto* m = static_cast<Machine*>(ud);
    m->watch_hit_ = true;
    m->watch_addr_ = (uint32_t)addr;
    m->b_->hit_breakpoint = true;                        // the slice goes on afterwards, as at a breakpoint
    uc_emu_stop(uc);
}

void Machine::rehook_watchpoints() {
    for (auto h : watch_hooks_)
        uc_hook_del(b_->uc, h);
    watch_hooks_.clear();
    for (auto& w : watch_) {
        int type = (w.kind & 1 ? UC_HOOK_MEM_WRITE : 0) | (w.kind & 2 ? UC_HOOK_MEM_READ : 0);
        uc_hook h;
        if (uc_hook_add(b_->uc, &h, type, (void*)watch_cb, this, w.addr, w.addr + w.len - 1) == UC_ERR_OK)
            watch_hooks_.push_back(h);
    }
}

void Machine::set_watchpoint(Watchpoint w, bool on) {
    call([&](Board&) {
        auto same = [&](const Watchpoint& x) { return x.addr == w.addr && x.len == w.len && x.kind == w.kind; };
        watch_.erase(std::remove_if(watch_.begin(), watch_.end(), same), watch_.end());
        if (on)
            watch_.push_back(w);
        rehook_watchpoints();
    });
}

std::vector<Watchpoint> Machine::watchpoints() {
    return call([this](Board&) { return watch_; });
}

void Machine::serial_send(int minor, std::vector<uint8_t> data) {
    post([minor, data = std::move(data)](Board& b) mutable { b.serial_send(minor, std::move(data)); });
}

void Machine::ir_press(const std::string& keys) {
    post([keys](Board& b) { b.ir_press(keys); });
}

}  // namespace sim
