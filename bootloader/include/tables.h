/* Data tables copied verbatim from BOOTSTRAP V2.00 (see tools/gen_tables.py). */
#ifndef TABLES_H
#define TABLES_H

#include <stdint.h>

extern const uint32_t *const upm_tables[3];
extern const uint32_t amx_by_cols[6];
extern const uint32_t refresh_div[6];
extern const uint32_t bank_bases[17];
extern const uint32_t nvram_sizes[4];
extern const uint32_t nvram_br[2];
extern const uint32_t flash_geom[16];
extern const uint8_t  flash_wait_fast[8];
extern const uint8_t  flash_wait_slow[8];

#endif
