// TzTable.cpp — manual-zone GMT ladder. See TzTable.h.
#include "TzTable.h"
#include <string.h>

const TzOpt TZ_TABLE[] = {
    // Manual mode = plain GMT offset picker: GMT-12..GMT+14 in half-hour
    // steps plus the three :45 zones (+5.75 Nepal, +8.75 Eucla, +12.75
    // Chatham std). Fixed offsets, no DST — auto (GNSS) mode handles named
    // zones and summer time. Generated + verified against tz_offset_at().
    {"GMT-12", "<-12>12"},
    {"GMT-11.5", "<-1130>11:30"},
    {"GMT-11", "<-11>11"},
    {"GMT-10.5", "<-1030>10:30"},
    {"GMT-10", "<-10>10"},
    {"GMT-9.5", "<-0930>9:30"},
    {"GMT-9", "<-09>9"},
    {"GMT-8.5", "<-0830>8:30"},
    {"GMT-8", "<-08>8"},
    {"GMT-7.5", "<-0730>7:30"},
    {"GMT-7", "<-07>7"},
    {"GMT-6.5", "<-0630>6:30"},
    {"GMT-6", "<-06>6"},
    {"GMT-5.5", "<-0530>5:30"},
    {"GMT-5", "<-05>5"},
    {"GMT-4.5", "<-0430>4:30"},
    {"GMT-4", "<-04>4"},
    {"GMT-3.5", "<-0330>3:30"},
    {"GMT-3", "<-03>3"},
    {"GMT-2.5", "<-0230>2:30"},
    {"GMT-2", "<-02>2"},
    {"GMT-1.5", "<-0130>1:30"},
    {"GMT-1", "<-01>1"},
    {"GMT-0.5", "<-0030>0:30"},
    {"GMT", "GMT0"},
    {"GMT+0.5", "<+0030>-0:30"},
    {"GMT+1", "<+01>-1"},
    {"GMT+1.5", "<+0130>-1:30"},
    {"GMT+2", "<+02>-2"},
    {"GMT+2.5", "<+0230>-2:30"},
    {"GMT+3", "<+03>-3"},
    {"GMT+3.5", "<+0330>-3:30"},
    {"GMT+4", "<+04>-4"},
    {"GMT+4.5", "<+0430>-4:30"},
    {"GMT+5", "<+05>-5"},
    {"GMT+5.5", "<+0530>-5:30"},
    {"GMT+5.75", "<+0545>-5:45"},
    {"GMT+6", "<+06>-6"},
    {"GMT+6.5", "<+0630>-6:30"},
    {"GMT+7", "<+07>-7"},
    {"GMT+7.5", "<+0730>-7:30"},
    {"GMT+8", "<+08>-8"},
    {"GMT+8.5", "<+0830>-8:30"},
    {"GMT+8.75", "<+0845>-8:45"},
    {"GMT+9", "<+09>-9"},
    {"GMT+9.5", "<+0930>-9:30"},
    {"GMT+10", "<+10>-10"},
    {"GMT+10.5", "<+1030>-10:30"},
    {"GMT+11", "<+11>-11"},
    {"GMT+11.5", "<+1130>-11:30"},
    {"GMT+12", "<+12>-12"},
    {"GMT+12.5", "<+1230>-12:30"},
    {"GMT+12.75", "<+1245>-12:45"},
    {"GMT+13", "<+13>-13"},
    {"GMT+13.5", "<+1330>-13:30"},
    {"GMT+14", "<+14>-14"},
};
const uint16_t TZ_COUNT = (uint16_t)(sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]));

const char TZ_OPTS[] =
    "GMT-12\n"
    "GMT-11.5\n"
    "GMT-11\n"
    "GMT-10.5\n"
    "GMT-10\n"
    "GMT-9.5\n"
    "GMT-9\n"
    "GMT-8.5\n"
    "GMT-8\n"
    "GMT-7.5\n"
    "GMT-7\n"
    "GMT-6.5\n"
    "GMT-6\n"
    "GMT-5.5\n"
    "GMT-5\n"
    "GMT-4.5\n"
    "GMT-4\n"
    "GMT-3.5\n"
    "GMT-3\n"
    "GMT-2.5\n"
    "GMT-2\n"
    "GMT-1.5\n"
    "GMT-1\n"
    "GMT-0.5\n"
    "GMT\n"
    "GMT+0.5\n"
    "GMT+1\n"
    "GMT+1.5\n"
    "GMT+2\n"
    "GMT+2.5\n"
    "GMT+3\n"
    "GMT+3.5\n"
    "GMT+4\n"
    "GMT+4.5\n"
    "GMT+5\n"
    "GMT+5.5\n"
    "GMT+5.75\n"
    "GMT+6\n"
    "GMT+6.5\n"
    "GMT+7\n"
    "GMT+7.5\n"
    "GMT+8\n"
    "GMT+8.5\n"
    "GMT+8.75\n"
    "GMT+9\n"
    "GMT+9.5\n"
    "GMT+10\n"
    "GMT+10.5\n"
    "GMT+11\n"
    "GMT+11.5\n"
    "GMT+12\n"
    "GMT+12.5\n"
    "GMT+12.75\n"
    "GMT+13\n"
    "GMT+13.5\n"
    "GMT+14";

int tztable_index_of_posix(const char *posix) {
  if (!posix)
    return -1;
  for (uint16_t i = 0; i < TZ_COUNT; i++)
    if (strcmp(TZ_TABLE[i].posix, posix) == 0)
      return (int)i;
  return -1;
}
