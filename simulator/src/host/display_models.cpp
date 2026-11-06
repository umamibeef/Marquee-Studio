#include "display_models.h"

namespace host {

const std::vector<DisplayModel>& display_models() {
    static const std::vector<DisplayModel> m = {
        {0, 0x80C0, "AD0201", 80, 16},   {1, 0x80C1, "AD0202", 80, 32},     {2, 0x80C2, "AD0203", 80, 48},
        {3, 0x90C2, "AD0203IM", 80, 48}, {4, 0x8140, "AD0301", 120, 16},    {5, 0x8141, "AD0302", 120, 32},
        {6, 0x8142, "AD0303", 120, 48},  {7, 0x81C0, "AD0401", 160, 16},    {8, 0x81C1, "AD0402", 160, 32},
        {9, 0x81C2, "AD0403", 160, 48},  {10, 0x8240, "AD0501", 200, 16},   {11, 0x8241, "AD0502", 200, 32},
        {12, 0x8242, "AD0503", 200, 48}, {13, 0x81C3, "AD0404", 160, 64},   {14, 0x8243, "AD0504", 200, 64},
        {15, 0x8244, "AD0505", 200, 80}, {16, 0xD0C2, "ED0203IM", 80, 48},  {17, 0xC141, "ED0302", 120, 32},
        {18, 0xC142, "ED0303", 120, 48}, {19, 0xC1C1, "ED0402", 160, 32},   {20, 0xC1C2, "ED0403", 160, 48},
        {21, 0xC1C3, "ED0404", 160, 64}, {22, 0xC241, "ED0502", 200, 32},   {23, 0xC242, "ED0503", 200, 48},
        {24, 0xC243, "ED0504", 200, 64}, {25, 0xC244, "ED0505", 200, 80},
        // not in the firmware's table (the menu cannot pick it), but display_config_read takes any word that fits
        // the driver frame: this sign's config EEPROM holds 0xC240 (read with a Saleae)
        {-1, 0xC240, "ED0501", 200, 16},
    };
    return m;
}

}  // namespace host
