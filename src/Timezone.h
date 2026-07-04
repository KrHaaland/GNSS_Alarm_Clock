// Timezone.h — offline timezone-from-coordinates lookup + POSIX TZ evaluator.
//
// tz_lookup(): maps lat/lon to a timezone using an embedded table of
// bounding boxes (Europe fine-grained, rest of the world coarse), ordered
// most-specific-first. Falls back to a pure longitude offset (no DST) when
// no box matches. Returns a POSIX TZ string with DST rules in "M" form,
// e.g. "CET-1CEST,M3.5.0,M10.5.0/3".
//
// tz_offset_at(): evaluates such a POSIX string at a given UTC instant and
// returns the local offset in seconds (east positive), handling DST
// transitions correctly (transitions are specified in local time).
// This module is hardware-independent (no Arduino.h) so it can be unit
// tested on the host with `pio test -e native`.
#pragma once
#include <stdint.h>
#include <time.h>

struct TzResult {
  char name[20];   // human readable, e.g. "Europe/Oslo" or "UTC+3"
  char posix[48];  // POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
};

// Look up timezone for coordinates. Always succeeds (longitude fallback).
void tz_lookup(float lat, float lon, TzResult &out);

// Offset from UTC in seconds (east positive) for `posix` at UTC time `utc`.
// If isDst is non-null it receives whether DST is active.
// Returns 0 and sets isDst=false if the string is unparseable.
int32_t tz_offset_at(const char *posix, time_t utc, bool *isDst = nullptr);

// --- Civil time helpers (proleptic Gregorian, no libc tz dependency) ---
// days since 1970-01-01 from civil date (Howard Hinnant's algorithm)
int32_t civil_to_days(int y, int m, int d);
void days_to_civil(int32_t z, int &y, int &m, int &d);
// Break a UTC/local epoch into fields. wday: 0=Sunday.
void epoch_to_tm(time_t t, int &year, int &mon, int &day, int &hour, int &min,
                 int &sec, int &wday);
time_t tm_to_epoch(int year, int mon, int day, int hour, int min, int sec);
