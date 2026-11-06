#include "disasm.h"

#include <capstone/capstone.h>

#include <cstdio>

namespace sim {

Disasm::Disasm() {
    csh h = 0;
    if (cs_open(CS_ARCH_PPC, (cs_mode)(CS_MODE_32 | CS_MODE_BIG_ENDIAN), &h) == CS_ERR_OK)
        handle_ = h;
}

Disasm::~Disasm() {
    if (handle_) {
        csh h = handle_;
        cs_close(&h);
    }
}

std::string Disasm::one(uint32_t addr, uint32_t w, const Symbols* names) const {
    uint8_t b[4] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w};
    cs_insn* insn = nullptr;
    size_t n = handle_ ? cs_disasm(handle_, b, 4, addr, 1, &insn) : 0;
    char buf[160];
    if (!n) {
        snprintf(buf, sizeof buf, ".long 0x%08x", w);
        return buf;
    }
    std::string text = std::string(insn[0].mnemonic) + " " + insn[0].op_str;
    cs_free(insn, n);
    // a direct branch (b, bl, bc...): name its target
    uint32_t op = w >> 26;
    if (names && (op == 18 || op == 16)) {
        uint32_t target;
        if (op == 18) {
            int32_t li = (int32_t)(w & 0x03FFFFFC);
            if (li & 0x02000000)
                li -= 0x04000000;
            target = (w & 2) ? (uint32_t)li : addr + li;
        } else {
            int32_t bd = (int32_t)(w & 0xFFFC);
            if (bd & 0x8000)
                bd -= 0x10000;
            target = (w & 2) ? (uint32_t)bd : addr + bd;
        }
        std::string nm = names->describe(target);
        if (!nm.empty())
            text += "  <" + nm + ">";
    }
    return text;
}

}  // namespace sim
