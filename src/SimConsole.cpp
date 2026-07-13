// SimConsole.cpp — simulated-GNSS serial console. Active only in env:sim
// (-DGNSS_SIM); otherwise this file compiles to nothing.
//
// Commands (type into the USB serial monitor, 115200):
//   pos <lat> <lon>              set a fix, resolve + apply the timezone
//   tz  <lat> <lon>              query the timezone only (no state change)
//   utc <Y> <M> <D> <h> <m> <s>  set the UTC clock (so local time shows)
//   status                       print sim state + current local time
//   nofix                        clear the simulated fix
//   help                         list commands
#include "SimConsole.h"

#ifdef GNSS_SIM
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "ClockKeeper.h"
#include "Gnss.h"
#include "Settings.h"
#include "Timezone.h"

static char s_buf[100];
static uint8_t s_n = 0;

// Print a UTC-offset in seconds as "+H:MM" / "-H:MM".
static void print_off(int32_t secs) {
  Serial.print(secs < 0 ? '-' : '+');
  uint32_t a = secs < 0 ? (uint32_t)-secs : (uint32_t)secs;
  Serial.print(a / 3600);
  Serial.print(':');
  uint32_t m = (a % 3600) / 60;
  if (m < 10)
    Serial.print('0');
  Serial.print(m);
}

// Hand-rolled parse/print for decimal degrees — avoids pulling libc's float
// strtof / dtostrf into the image (the rest of the firmware sidesteps them too,
// and on the 512 KB part there's no room to spare).
static float parse_deg(const char *s) {
  float sign = 1.0f;
  if (*s == '-') { sign = -1.0f; s++; }
  else if (*s == '+') { s++; }
  long ip = 0;
  while (*s >= '0' && *s <= '9') ip = ip * 10 + (*s++ - '0');
  float frac = 0.0f, scale = 0.1f;
  if (*s == '.') {
    s++;
    while (*s >= '0' && *s <= '9') { frac += (*s++ - '0') * scale; scale *= 0.1f; }
  }
  return sign * ((float)ip + frac);
}

static void print_deg(float v) {
  if (v < 0) { Serial.print('-'); v = -v; }
  long ip = (long)v;
  long frac = (long)((v - (float)ip) * 10000.0f + 0.5f);
  if (frac >= 10000) { ip++; frac -= 10000; }
  Serial.print(ip);
  Serial.print('.');
  for (long p = 1000; p > 1 && frac < p; p /= 10) Serial.print('0');
  Serial.print(frac);
}

static void fmt_dt(time_t t, char *b, size_t n) {
  int y, mo, d, h, mi, s, wd;
  epoch_to_tm(t, y, mo, d, h, mi, s, wd);
  snprintf(b, n, "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
}

// Resolve + report the zone for lat/lon; if apply, copy it into settings so the
// on-screen clock reflects it immediately (bypasses ClockKeeper's 10-min throttle).
static void show_tz(float lat, float lon, bool apply) {
  TzResult r;
  tz_lookup(lat, lon, r);
  Serial.print("  zone: ");
  Serial.print(r.name);
  Serial.print("  [");
  Serial.print(r.posix);
  Serial.println("]");
  bool dj, du;
  int32_t j = tz_offset_at(r.posix, tm_to_epoch(2026, 1, 15, 12, 0, 0), &dj);
  int32_t u = tz_offset_at(r.posix, tm_to_epoch(2026, 7, 15, 12, 0, 0), &du);
  Serial.print("  Jan UTC");
  print_off(j);
  if (dj)
    Serial.print(" (DST)");
  Serial.print("   Jul UTC");
  print_off(u);
  if (du)
    Serial.print(" (DST)");
  Serial.println();
  if (apply) {
    strncpy(settings().tzPosix, r.posix, TZ_POSIX_LEN - 1);
    settings().tzPosix[TZ_POSIX_LEN - 1] = '\0';
    strncpy(settings().tzName, r.name, TZ_NAME_LEN - 1);
    settings().tzName[TZ_NAME_LEN - 1] = '\0';
  }
}

static void print_status() {
  float lat, lon;
  Serial.print("[sim] fix: ");
  if (gnss_get_position(lat, lon)) {
    print_deg(lat);
    Serial.print(", ");
    print_deg(lon);
  } else {
    Serial.print("none");
  }
  Serial.print("   hasFix=");
  Serial.println(gnss_has_fix() ? "yes" : "no");
  if (clock_valid()) {
    char b[24];
    fmt_dt(clock_now_utc(), b, sizeof(b));
    Serial.print("  UTC  : ");
    Serial.println(b);
    fmt_dt(clock_now_local(), b, sizeof(b));
    Serial.print("  local: ");
    Serial.print(b);
    Serial.print("  (");
    Serial.print(settings().tzName);
    Serial.print(" UTC");
    print_off(clock_tz_offset());
    if (clock_is_dst())
      Serial.print(" DST");
    Serial.println(")");
  } else {
    Serial.println("  clock: not set (use 'utc <Y> <M> <D> <h> <m> <s>')");
  }
}

static void help() {
  Serial.println("sim commands:");
  Serial.println("  pos <lat> <lon>              set fix, resolve + apply timezone");
  Serial.println("  tz  <lat> <lon>              query timezone only (no change)");
  Serial.println("  utc <Y> <M> <D> <h> <m> <s>  set UTC clock");
  Serial.println("  mov <kmh> [altM]             feed speed/altitude (modes)");
  Serial.println("  status                       show sim state + local time");
  Serial.println("  nofix                        clear the simulated fix");
  Serial.println("  help                         this list");
}

static void handle(char *line) {
  char *cmd = strtok(line, " \t");
  if (!cmd)
    return;
  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
    help();
  } else if (!strcmp(cmd, "pos") || !strcmp(cmd, "tz")) {
    char *a = strtok(NULL, " \t,");
    char *b = strtok(NULL, " \t,");
    if (!a || !b) {
      Serial.println("usage: pos <lat> <lon>");
      return;
    }
    float lat = parse_deg(a), lon = parse_deg(b);
    bool apply = !strcmp(cmd, "pos");
    if (apply) {
      gnss_sim_set_fix(lat, lon);
      Serial.print("[sim] fix @ ");
      Serial.print(a); // echo the raw input (no float printing)
      Serial.print(", ");
      Serial.println(b);
    }
    show_tz(lat, lon, apply);
  } else if (!strcmp(cmd, "utc")) {
    char *t[6];
    int k = 0, ok = 1;
    char *p;
    while (k < 6 && (p = strtok(NULL, " \t:-/")) != NULL)
      t[k++] = p;
    if (k < 6) {
      Serial.println("usage: utc <Y> <M> <D> <h> <m> <s>");
      return;
    }
    (void)ok;
    time_t e = tm_to_epoch(atoi(t[0]), atoi(t[1]), atoi(t[2]), atoi(t[3]),
                           atoi(t[4]), atoi(t[5]));
    gnss_sim_set_utc(e);
    clock_request_sync();
    char b[24];
    fmt_dt(e, b, sizeof(b));
    Serial.print("[sim] UTC set ");
    Serial.println(b);
  } else if (!strcmp(cmd, "mov")) {
    char *a = strtok(NULL, " \t,");
    char *bb = strtok(NULL, " \t,");
    if (!a) {
      Serial.println("usage: mov <kmh> [altM]");
      return;
    }
    float kmh = parse_deg(a);
    float alt = bb ? parse_deg(bb) : 0.0f;
    gnss_sim_set_motion(kmh, alt);
    Serial.print("[sim] speed ");
    Serial.print(a);
    Serial.print(" km/h, alt ");
    Serial.print(bb ? bb : "0");
    Serial.println(" m (needs a fix: use 'pos' too)");
  } else if (!strcmp(cmd, "nofix")) {
    gnss_sim_clear_fix();
    Serial.println("[sim] fix cleared");
  } else if (!strcmp(cmd, "status")) {
    print_status();
  } else {
    Serial.print("? unknown command: ");
    Serial.println(cmd);
  }
}

void sim_console_begin() {
  Serial.println();
  Serial.println("=== GNSS SIM MODE (no real GPS) — type 'help' ===");
  help();
}

void sim_console_task() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (s_n) {
        s_buf[s_n] = '\0';
        handle(s_buf);
        s_n = 0;
      }
    } else if (s_n < sizeof(s_buf) - 1) {
      s_buf[s_n++] = c;
    }
  }
}
#endif // GNSS_SIM
