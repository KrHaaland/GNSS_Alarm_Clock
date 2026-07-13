// TzTable.h — the manual-zone GMT offset ladder, shared by the UI (dropdown)
// and Settings (packs the selection as an index into the RTC user EEPROM).
#pragma once
#include <stdint.h>

struct TzOpt {
  const char *name;
  const char *posix;
};

extern const TzOpt TZ_TABLE[];
extern const uint16_t TZ_COUNT;
extern const char TZ_OPTS[]; // '\n'-joined names for the LVGL dropdown

// Index of the entry whose POSIX string equals `posix`; -1 if none.
int tztable_index_of_posix(const char *posix);
