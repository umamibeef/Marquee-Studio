// The emulated sign: an MPC862 board running the real boot ROM and ELITE_RGB (RTEMS included) under Unicorn.
//
// Boot: the boot ROM runs from reset (FLASH + 0x100) to the jump into the application at 0, with the memory
// controller, the I2C config EEPROMs and the hand-off block. The MPC8xx special instructions (mtspr/mfspr/mtmsr/
// mfmsr/tlbia) are intercepted and kept in a shadow MSR and SPR table, so Unicorn never turns translation on.
//
// App: the same, over the app's code (plus rfi and mftb), and the devices the app uses: the SIU and CPM interrupt
// controllers, the PIT (RTEMS's clock tick), the CPM UARTs behind /dev/rs232 and /dev/rs485, IDMA channel 1 as the
// LED panel output, the I2C EEPROMs, and the IR receiver on PC13. Interrupts are delivered between execution slices
// and when a handler returns (rfi), as the CPU does. Time is a fixed number of instructions per microsecond.
//
// Instructions are counted per translated block (a block hook), and a slice that would end inside a block runs that
// block's first instructions on their own, so every slice ends on the same instruction whatever the block layout.
#pragma once

#include <unicorn/unicorn.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sim {

constexpr uint32_t FLASH = 0xF8180000, IMMR = 0xFF000000, NVRAM = 0xF8780000, MAIN = 0x50000000, CS7 = 0x60000000;
constexpr uint32_t RAM_SIZE = 0x01000000;          // what the boot ROM model maps first
constexpr uint32_t RAM_TOTAL = 0x08000000;         // all 128 MB the config EEPROM declares
constexpr uint32_t CODE_END = 0x988C4;             // end of the app's code
constexpr uint64_t CPU_MIPS = 40;                  // instructions per microsecond, nominal
constexpr uint64_t SLICE = 200000;                 // instructions between interrupt checks
constexpr uint64_t SLICE_IR = 10000;               // ... while remote-control edges are being delivered
constexpr uint32_t STAGE_VIRT = 0x07F80000;       // our boot loader's RAM stage, as the firmware sees it (virtual)
constexpr uint32_t STAGE_PHYS = 0x0EF80000;       // ... and where the boot loader writes it (physical)
constexpr uint32_t STAGE_BYTES = 0x00080000;
constexpr uint64_t BOOT_MAX = 2000000000;        // a slot boot (window, RAM-stage copy, unpacking) runs long
constexpr uint32_t IR_ISR_LO = 0x59608, IR_ISR_HI = 0x59738;   // ir_edge_isr: its mftb reads get the edge times
constexpr uint64_t IR_KEY_GAP_US = 300000;         // at least this long between key presses

constexpr uint64_t EE = 0x8000, PR = 0x4000, RI = 0x2;
constexpr int SPR_SRR0 = 26, SPR_SRR1 = 27, SPR_TBL_R = 268, SPR_TBU_R = 269, SPR_EIE = 80, SPR_EID = 81, SPR_NRI = 82;

// The IR remote's key table in the app (0xBB048): the character for each key code.
extern const char KEYMAP[];

struct Scenario {                                  // board straps and NVRAM boot counter (cosim.SCENARIOS)
    bool spd = false;
    uint8_t tries = 0xFF;
    uint8_t pipr = 0x02;
    uint32_t pbdat = 0x3C000;
    uint32_t pddat = 0x0000;
    // Config EEPROM display geometry (set_geometry). 80 x 16 (AD0201, the firmware's fallback) keeps the
    // verification fingerprints; the real sign's EEPROM holds 0xC240 (ED0501: 200 x 16, E-series).
    uint16_t geom_word = 0x80C0;
    int geom_w = 80, geom_h = 16;
};

struct Symbols;

class Board {
public:
    Board(const std::vector<uint8_t>& flash, Scenario sc, std::vector<uint8_t> app);
    ~Board();
    Board(const Board&) = delete;
    Board& operator=(const Board&) = delete;

    // The boot ROM, from reset to the app at 0. False if it never got there.
    bool boot();
    // Put the app image in RAM in place of what the boot ROM unpacked (the stock app from the dump), keeping what the
    // boot ROM wrote into it on purpose: the hand-off block 0x14..0x9C and the MMU table (pointer/length at 0x4C).
    void load_app_image();
    // The app's special instructions, the busy-wait skip and the context-switch watch (needs symbols).
    int hook_app_specials();
    void fake_fpga(const Symbols& S);               // the panel FPGA that board type 2's firmware loads
    void on_fpga_call(uint64_t addr);
    void hook_busy_waits(const Symbols& S);
    void watch_switches(const Symbols& S);

    // Run until icount reaches max_insns, slice by slice, as the Python simulator did.
    void run_app(uint64_t max_insns);
    // One step of the run loop for the debugger: up to n instructions of the current slice. At a slice's end the
    // devices tick and pending interrupts are taken. Returns the instructions run (fewer at a breakpoint).
    uint64_t advance(uint64_t n);
    // Instructions run so far, exactly (icount counts whole slices).
    uint64_t exact_count() const { return icount + slice_done; }
    uint64_t slice_left() const { return slice_size - slice_done; }

    // Inputs.
    void serial_send(int minor, std::vector<uint8_t> data, uint64_t at = 0, uint64_t at_frame = 0);
    void ir_press(const std::string& keys, uint64_t at_frame = 0);
    void force_config_word(uint32_t addr, uint32_t value);
    void force_config_bytes(uint32_t addr, std::vector<uint8_t> data);
    void watch_writes(uint32_t addr, uint32_t len);   // records each new value written there (states)
    // fn before each run of the instruction at addr (the tests watch calls with it)
    void on_exec(uint32_t addr, std::function<void(Board&)> fn);

    // Call the firmware function at addr in the paused system and return r3 (the tests use it). EABI: args go in
    // r3..r10, the rest in the caller's frame from sp + 8; it returns to CALL_RETURN. Every register and the
    // instruction counter are restored afterwards, so a run can go on. Throws std::runtime_error when the
    // emulation fails or the function does not return within max_insns.
    static constexpr uint32_t CALL_RETURN = 0x3C;  // in the vector area: no call returns there
    uint32_t call(uint32_t addr, const std::vector<uint32_t>& args, uint32_t sp, uint64_t max_insns = 20000000);

    // Breakpoints: execution stops before the instruction at the address.
    std::set<uint32_t> breakpoints;
    bool hit_breakpoint = false;
    uint32_t hit_pc = 0;

    // Registers and memory.
    uint32_t reg(int r) const;
    void set_reg(int r, uint32_t v);
    uint32_t pc() const;
    uint32_t r32(uint32_t a) const;
    bool read(uint32_t a, void* out, size_t n) const;
    bool write(uint32_t a, const void* in, size_t n);
    bool write_code(uint32_t a, const void* in, size_t n);
    uint64_t now() const { return icount + skew; }

    // ---- state (what the Python simulator's fingerprint covers) ----
    uc_engine* uc = nullptr;
    Scenario sc;
    std::vector<uint8_t> app;
    uint32_t tcn2 = 0;                              // CPM timer 2 counter, advanced per read (boot-loader windows)
    bool exceptions = true;                         // faults go to the firmware's vectors (take_exception)
    uint32_t fault_count = 0;
    bool fpga_model = false, fpga_done = false;     // fake_fpga()
    uint32_t fpga_len = 0;
    std::map<uint32_t, std::string> fpga_calls;
    bool reset_requested = false;                   // the app set PLPRCR CSR (cpu_reset_maybe)
    bool swsr_armed = false;                        // SWSR: 0x556C seen, waiting for 0xAA39
    std::optional<uint64_t> wdt_last;               // when the watchdog was last serviced (now())
    static constexpr uint64_t WDT_TIMEOUT_US = 1340000;
    std::map<int, std::vector<uint8_t>> eeproms;
    std::vector<uint8_t> dpram;                     // IMMR + DPRAM backing store (64 KB)
    std::map<int, uint32_t> spr;
    uint64_t msr = 0;                               // the shadow MSR
    uint64_t icount = 0, skew = 0;
    std::optional<uint64_t> pit_next;
    std::vector<std::vector<uint8_t>> frames;       // every panel frame (RGB555 big-endian)
    size_t frames_kept = SIZE_MAX;                  // keep only the last n (the GUI); frame_total counts all
    uint64_t frame_total = 0;
    struct Rx { uint64_t at, at_frame; int minor; std::vector<uint8_t> data; };
    std::deque<Rx> rx_queue;
    std::map<int, int> rx_idx, tx_idx;
    std::map<int, std::vector<uint8_t>> serial_tx;
    std::deque<std::pair<uint64_t, char>> ir_queue;
    std::deque<std::pair<int, uint64_t>> ir_edges;
    int ir_level = 1;
    uint64_t ir_tb = 0, ir_keys_sent = 0;
    std::optional<uint64_t> ir_last;
    std::map<std::string, uint64_t> events;
    std::map<std::string, uint64_t> tasks;          // context switches per task name
    std::map<std::string, uint32_t> tcbs;           // task name -> TCB
    std::vector<std::pair<uint64_t, std::string>> switches;   // the first 40

    // The trace for the timeline view (off unless asked for: it changes nothing the fingerprint covers). Times are
    // instructions, exact to the translated block (now() + the part of the slice run).
    enum TraceKind : uint8_t { TR_TASK, TR_IRQ, TR_PIT, TR_FRAME, TR_RX, TR_TX, TR_IR, TR_STOP };
    struct TraceEvent {
        uint64_t t;
        uint8_t kind, a;                                 // a: the serial minor, or the IR key
        uint16_t b;                                      // the task's name index, or the byte count
    };
    bool trace_on = false;
    std::vector<TraceEvent> trace;                       // drained by the Machine
    std::vector<std::string> trace_names;                // task names, by index
    void tr(uint8_t kind, uint8_t a = 0, uint16_t b = 0) {
        if (trace_on && trace.size() < (1u << 22))
            trace.push_back({now() + slice_done, kind, a, b});
    }
    std::vector<uint32_t> states;                   // watch_writes
    size_t reg_writes = 0;                          // IMMR register writes during the boot
    bool reached_app = false, in_app = false;
    std::vector<uint8_t> stage_ram;                  // the RAM stage, mapped at STAGE_VIRT and STAGE_PHYS

    // ---- the counting core (sim/fastcount.c, now native) ----
    struct Count {
        uint64_t count = 0, limit = UINT64_MAX, block = 0, block_n = 0, stop_pc = 0, stop_n = 0;
        bool stopped = false;
    } cnt;
    bool counting = false;
    void skip_rest(uint64_t addr);
    uint64_t emu(uint64_t pc, uint64_t n);

    // hook entry points (public for the C callbacks)
    // Main flash (2 x AM29LV128 on CS4/CS5, 16 MB each, word mode): erased reads 0xFF; the AMD command set (unlock,
    // program = AND into the cell, 64 KB sector erase, chip erase, reset) completes at once, so status polling sees the
    // final data as soon as it looks. main_flash[] is its content.
    std::vector<uint8_t> main_flash = std::vector<uint8_t>(0x2000000, 0xFF);
    int mf_state[2] = {0, 0};
    uint64_t main_read(uint64_t off, unsigned size);
    void main_write(uint64_t off, unsigned size, uint64_t value);
    uint64_t immr_read(uint64_t off, unsigned size);
    void immr_write(uint64_t off, unsigned size, uint64_t value);
    void special_boot(uint64_t addr);
    void special_app(uint64_t addr);
    void rfi(uint64_t addr);
    void block(uint64_t addr, uint32_t size);
    void done(uint64_t addr);
    void busy_wait(uint64_t addr);
    void on_switch(uint64_t addr);

private:
    uint64_t slice_size = 0, slice_done = 0;
    std::vector<uc_hook> hooks;
    struct Force { uint32_t addr; std::vector<uint8_t> data; Board* b; };
    std::vector<Force*> forces;
    struct WriteWatch { Board* b; };
    WriteWatch* write_watch = nullptr;
    struct ExecHook { Board* b; std::function<void(Board&)> fn; };
    std::vector<std::unique_ptr<ExecHook>> exec_hooks;
    bool call_hooked = false;

    void add_code_hook(void* cb, uint64_t begin, uint64_t end);
    void i2c_boot();
    void i2c_app();
    uint32_t dp32(uint32_t off) const;
    void dp32(uint32_t off, uint32_t v);
    uint16_t dp16(uint32_t off) const;
    void dp16(uint32_t off, uint16_t v);
    int cpm_highest() const;
    int sivec() const;
    void set_sipend(int bit, bool on);
    void tick_idma();
    void tick_cpm();
    void tick_serial();
    void tick_ir();
    void tick_devices();
    void levels();
    bool take_exception(uc_err e);
    void maybe_interrupt();
    void serial_event(int minor, int bits);
    std::vector<std::pair<int, uint64_t>> ir_frame(char ch);
    bool bp_in_block(uint64_t addr, uint64_t n) const;
};

std::vector<uint8_t> eeprom_cfg(bool spd);
std::vector<uint8_t> eeprom_a2(bool spd);
void set_geometry(std::vector<uint8_t>& eeprom, uint16_t word = 0x80C0, int w = 80, int h = 16);
std::string task_name(const Board& b, uint32_t tcb);

}  // namespace sim
