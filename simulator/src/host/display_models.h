// The display models a config EEPROM can name (for umami-marquee-sim --model).
#pragma once
#include <cstdint>
#include <vector>

namespace host {

struct DisplayModel {
    int type;
    uint16_t word;
    const char* name;
    int w, h;
};
// The geometry table 0x992B8, plus ED0501 (0xC240, type -1): not in the table, but it is what this sign's config
// EEPROM holds (200 x 16, E-series), and display_config_read accepts it. AD0201 is the firmware's fallback.
const std::vector<DisplayModel>& display_models();

}  // namespace host
