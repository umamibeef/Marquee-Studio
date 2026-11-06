// umami-marquee-sim-test: the simulator's verification runs (the C++ port of sim/compare*.py, flashtest.py,
// find_culprit.py, test_console.py and test_native_emoji.py). Shared set-ups and report helpers.
#pragma once

#include "board.h"
#include "repo.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sim {

using Frame = std::vector<uint8_t>;

// The repository (found from the working directory upwards, or set with --repo DIR before the subcommand).
const Repo& repo();
// An instruction count from the command line ("80000000" or "80_000_000").
uint64_t count_arg(const std::string& s);

// The set-up of sim/compare*.py: the stock boot flash boots, then the app image replaces the unpacked stock app;
// the special instructions, the busy-wait skip and the context-switch watch are hooked.
std::unique_ptr<Board> swapped(const std::string& app_path);
// The set-up of sim/flashtest.py: the whole boot-flash image boots as on the sign, nothing is swapped in. bad gets
// the RAM words (offsets) that differ from app after the hand-off, except the hand-off block and the MMU table.
std::unique_ptr<Board> booted(const std::vector<uint8_t>& flash, const std::vector<uint8_t>& app,
                              std::vector<uint32_t>* bad = nullptr);

// compare.distinct: the 80x16 frames with bit 15 (the driver's marker) cleared, consecutive repeats dropped.
std::vector<Frame> distinct(const std::vector<Frame>& frames);
Frame masked(const Frame& f);

// Python-style text for the reports (the scripts print lists and dicts as Python does).
std::string py_tasks(const Board& b);                    // sorted(b.tasks)
std::string py_bytes_repr(const std::vector<uint8_t>& latin1);   // repr() of a Latin-1 str
std::string hex(const std::vector<uint8_t>& v, const char* sep = "");
std::string latin1_to_utf8(const std::vector<uint8_t>& v);
uint64_t event(const Board& b, const std::string& name);  // b.events.get(name, 0)

// The DB9 download protocol (studio/marquee_studio/signproto.py): host frames, and the sign's replies.
std::vector<uint8_t> db9_frame(uint8_t type, const std::vector<uint8_t>& data = {}, uint16_t param = 0,
                               uint8_t sync = 0x5A);
struct SignFrame {
    uint8_t type;
    uint16_t val;
    std::vector<uint8_t> data;
    bool ok;
};
std::vector<SignFrame> parse_sign(const std::vector<uint8_t>& buf);
std::string describe(const SignFrame& f);

// appsim.save_frames: frames (RGB555 big-endian) as PNGs in out: sheet.png (up to 24, 4x) and last.png (8x).
void save_frames(const std::vector<Frame>& frames, const std::string& out, int W = 0, int H = 0);

// The subcommands; each returns the exit status.
int cmd_compare(const std::vector<std::string>& args);
int cmd_compare_rs485(const std::vector<std::string>& args);
int cmd_compare_db9(const std::vector<std::string>& args);
int cmd_compare_menu(const std::vector<std::string>& args);
int cmd_flashtest(const std::vector<std::string>& args);
int cmd_find_culprit(const std::vector<std::string>& args);
int cmd_console(const std::vector<std::string>& args);
int cmd_native_emoji(const std::vector<std::string>& args);

}  // namespace sim
