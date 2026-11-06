// The simulated sign under control, for the debugger and the GDB server.
//
// One emulation thread owns the Board from creation to destruction; every other thread (the UI, the GDB server)
// sends it work through a queue, which it runs between slices. Unicorn is never touched from two threads.
#pragma once

#include "board.h"
#include "repo.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sim {

struct Task {
    uint32_t tcb = 0, id = 0, state = 0, prio = 0;
    std::string name;
    bool current = false;
    uint32_t regs[38] = {};                              // r0..r31, pc, msr, cr, lr, ctr, xer (saved context if not current)
};

struct Watchpoint {
    uint32_t addr, len;
    int kind;                                            // 1 write, 2 read, 3 access
};

struct MachineConfig {
    std::string flash;                                   // boot flash image
    std::string app;                                     // app image put in place of the one the boot ROM unpacked (optional)
    bool cougar = false;                                 // RS-485 set up for Cougar, as the RS-485 comparison does
    uint16_t geom_word = 0x80C0;                         // display model in the config EEPROM (Scenario::geom_*)
    int geom_w = 80, geom_h = 16;                        // lit LED area (display_width/height)
    int panel_w = 0, panel_h = 0;                        // panel frame the IDMA ships (panel_width/height); 0 = same as lit
};

class Machine {
public:
    bool from_source() const { return from_source_; }   // our firmware, not the original
    Machine(const Repo& repo, MachineConfig cfg);
    ~Machine();

    // Run control (any thread).
    void run();
    void halt();
    void step(uint64_t n = 1);
    void step_over();
    void reset();
    // Change the display model in the config EEPROM and reboot the sign (no-op when it is already that model).
    void set_geometry(uint16_t word, int w, int h);
    bool running() const { return running_; }
    void quit();

    // Run fn on the emulation thread and wait for its result.
    template <class F>
    auto call(F fn) -> decltype(fn(std::declval<Board&>())) {
        using R = decltype(fn(std::declval<Board&>()));
        auto p = std::make_shared<std::promise<R>>();
        auto fut = p->get_future();
        post([p, fn](Board& b) {
            if constexpr (std::is_void_v<R>) {
                fn(b);
                p->set_value();
            } else
                p->set_value(fn(b));
        });
        return fut.get();
    }
    void post(std::function<void(Board&)> fn);

    // What the UI shows, refreshed by the emulation thread.
    struct View {
        bool running = false, booted = false;
        uint32_t pc = 0;
        uint32_t regs[38] = {};
        uint64_t icount = 0, frames = 0, now = 0;
        double mips = 0;                                 // emulated instructions per second (millions)
        std::vector<uint8_t> frame;                      // the latest panel frame (RGB555 big-endia
        int disp_w = 0, disp_h = 0;                      // lit LED area (display_width/height; EEPROM word)
        int panel_w = 0, panel_h = 0;                    // panel frame = IDMA buffer (panel_width/height; EEPROM 0x13)n)
        std::map<std::string, uint64_t> events;
        std::vector<Task> tasks;
        std::string stop_reason;
        uint32_t watch_addr = 0;                         // the address a watchpoint stopped on
        int watch_kind = 0;
        std::vector<uint8_t> serial[6];                  // everything sent on each serial minor
    };
    View view();

    // Memory as the CPU sees it (IMMR from the register model, without read side effects). False if unmapped.
    bool read_mem(uint32_t a, uint8_t* out, size_t n);
    bool write_mem(uint32_t a, const uint8_t* in, size_t n);
    std::vector<uint32_t> regs();                        // 38, as in Task::regs
    void set_reg(int i, uint32_t v);
    std::vector<Task> tasks();

    // Breakpoints and watchpoints.
    void set_breakpoint(uint32_t a, bool on);
    std::vector<uint32_t> breakpoints();
    void set_watchpoint(Watchpoint w, bool on);
    std::vector<Watchpoint> watchpoints();

    // Inputs.
    void serial_send(int minor, std::vector<uint8_t> data);
    void ir_press(const std::string& keys);

    std::atomic<bool> tracing{false};                    // record the timeline trace (Board::trace)
    // The trace events recorded since the last call, and the task names their indexes refer to.
    std::vector<Board::TraceEvent> take_trace(std::vector<std::string>* names);
    std::atomic<bool> realtime{true};                    // pace the sign at its real speed (else as fast as possible)
    // Called on the emulation thread when the machine stops (breakpoint, watchpoint, step, halt), with the pc, the
    // signal for gdb and the running task's id (1 before multitasking). It must not call back into the Machine.
    std::function<void(uint32_t pc, int signal, uint32_t tid)> on_stop;
    const Repo& repo;

private:
    bool from_source_ = false;
    MachineConfig cfg_;
    std::unique_ptr<Board> b_;
    std::thread thread_;
    std::mutex mu_, view_mu_;
    std::condition_variable cv_;
    std::deque<std::function<void(Board&)>> queue_;
    std::atomic<bool> running_{false}, quit_{false};
    View view_;
    std::vector<Board::TraceEvent> trace_;               // moved out of the Board at each refresh
    std::vector<std::string> trace_names_;
    std::vector<Watchpoint> watch_;
    std::vector<uc_hook> watch_hooks_;
    bool watch_hit_ = false;
    uint32_t watch_addr_ = 0;
    uint64_t temp_bp_ = UINT64_MAX;

    void loop();
    void boot();
    bool reboot(const char* why);                       // boot() when the machine resets: false (and stopped) if it fails
    void refresh(bool full);
    void stopped(const std::string& why, int sig);
    void rehook_watchpoints();
    std::vector<Task> tasks_now();
    friend void watch_cb(uc_engine*, uc_mem_type, uint64_t, int, int64_t, void*);
};

}  // namespace sim
