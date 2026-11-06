// The NVRAM debug record our boot loader and firmware keep (bootloader/include/dbg.h; tools/dbgrecord.py is the
// same decoder in Python): the last boots' reset causes and steps, and the last crash.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace host {

constexpr uint32_t DBG_OFFSET = 0x7C00, DBG_SIZE = 0x100 + 0x30 + 32 * 4;
constexpr uint8_t DBG_OPT_SLOT_DIRECT = 0x01;

struct DbgRecord {
    bool valid = false;
    uint32_t seq = 0;
    uint8_t options = 0;
    struct Boot { uint32_t seq, rsr, step, value; };
    std::vector<Boot> boots;                             // oldest first
    bool crashed = false;
    uint32_t c_seq = 0, source = 0, vector = 0, srr0 = 0, srr1 = 0, dar = 0, dsisr = 0, lr = 0, cr = 0, step = 0,
             value = 0, gpr[32] = {};
};

DbgRecord dbg_decode(const std::vector<uint8_t>& blob);
// A readable report; symbolize (optional) names code addresses (SRR0, LR).
std::string dbg_report(const DbgRecord& r, const std::function<std::string(uint32_t)>& symbolize = {});

}  // namespace host
