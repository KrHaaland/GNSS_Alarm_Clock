// Timezone.cpp — offline lat/lon -> timezone lookup + POSIX TZ evaluator.
// Hardware independent: compiled for both SAMD51 and the host test runner
// (pio test -e native). Must not include Arduino.h.
#include "Timezone.h"
#include <stdio.h>
#include <string.h>

// --- Civil time helpers -----------------------------------------------------
// Howard Hinnant's algorithms (howardhinnant.github.io/date_algorithms.html).

int32_t civil_to_days(int y, int m, int d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = (uint32_t)(y - era * 400);                    // [0,399]
  const uint32_t doy =
      (153u * (uint32_t)(m > 2 ? m - 3 : m + 9) + 2u) / 5u + (uint32_t)(d - 1);
  const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy; // [0,146096]
  return era * 146097 + (int32_t)doe - 719468;
}

void days_to_civil(int32_t z, int &y, int &m, int &d) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = (uint32_t)(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const uint32_t mp = (5u * doy + 2u) / 153u;
  d = (int)(doy - (153u * mp + 2u) / 5u + 1u);
  m = (int)(mp < 10u ? mp + 3u : mp - 9u);
  y = (int)yoe + (int)era * 400 + (m <= 2);
}

void epoch_to_tm(time_t t, int &year, int &mon, int &day, int &hour, int &min,
                 int &sec, int &wday) {
  int64_t tt = (int64_t)t;
  int64_t dd = tt / 86400;
  if (tt % 86400 < 0)
    dd--; // floor division for pre-1970 instants
  int32_t rem = (int32_t)(tt - dd * 86400); // [0,86399]
  hour = rem / 3600;
  rem -= hour * 3600;
  min = rem / 60;
  sec = rem - min * 60;
  days_to_civil((int32_t)dd, year, mon, day);
  wday = (int)((dd + 4) % 7); // 1970-01-01 was a Thursday (=4)
  if (wday < 0)
    wday += 7;
}

time_t tm_to_epoch(int year, int mon, int day, int hour, int min, int sec) {
  return (time_t)((int64_t)civil_to_days(year, mon, day) * 86400 +
                  (int64_t)hour * 3600 + (int64_t)min * 60 + sec);
}

// --- POSIX TZ evaluator -----------------------------------------------------
// Grammar: NAME[+|-]H[:MM[:SS]][DSTNAME[[+|-]H[:MM]][,Mm.w.d[/time],Mm.w.d[/time]]]
// POSIX sign convention: stored offset = UTC - local, so "CET-1" is UTC+1.

// Skip a zone designator: alphabetic run, or anything inside '<'...'>'.
static const char *skip_name(const char *p) {
  if (*p == '<') {
    p++;
    while (*p && *p != '>')
      p++;
    if (*p == '>')
      p++;
    return p;
  }
  while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
    p++;
  return p;
}

// Parse [+|-]H[:MM[:SS]] into seconds (sign as written). False if no digits.
static bool parse_hms(const char *&p, int32_t &out) {
  const char *q = p;
  int32_t sign = 1;
  if (*q == '+') {
    q++;
  } else if (*q == '-') {
    sign = -1;
    q++;
  }
  if (*q < '0' || *q > '9')
    return false;
  int32_t v[3] = {0, 0, 0};
  int i = 0;
  for (;;) {
    while (*q >= '0' && *q <= '9') {
      v[i] = v[i] * 10 + (*q - '0');
      q++;
    }
    if (i < 2 && q[0] == ':' && q[1] >= '0' && q[1] <= '9') {
      q++;
      i++;
    } else {
      break;
    }
  }
  out = sign * (v[0] * 3600 + v[1] * 60 + v[2]);
  p = q;
  return true;
}

struct MRule {
  int mon;      // 1..12
  int week;     // 1..5, 5 = last
  int dow;      // 0..6, 0 = Sunday
  int32_t time; // seconds after local midnight; may be <0 or >24h per POSIX
};

static bool parse_uint(const char *&p, int &out) {
  if (*p < '0' || *p > '9')
    return false;
  int v = 0;
  while (*p >= '0' && *p <= '9')
    v = v * 10 + (*p++ - '0');
  out = v;
  return true;
}

static bool parse_mrule(const char *&p, MRule &r) {
  if (*p != 'M')
    return false;
  p++;
  if (!parse_uint(p, r.mon) || *p != '.')
    return false;
  p++;
  if (!parse_uint(p, r.week) || *p != '.')
    return false;
  p++;
  if (!parse_uint(p, r.dow))
    return false;
  if (r.mon < 1 || r.mon > 12 || r.week < 1 || r.week > 5 || r.dow > 6)
    return false;
  r.time = 2 * 3600; // POSIX default transition time 02:00:00
  if (*p == '/') {
    p++;
    if (!parse_hms(p, r.time))
      return false;
  }
  return true;
}

static int days_in_month(int y, int m) {
  static const uint8_t dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
    return 29;
  return dim[m - 1];
}

// Local-time instant (seconds since epoch, in the rule's own local frame)
// of a Mm.w.d[/time] transition in `year`.
static int64_t rule_local_secs(int year, const MRule &r) {
  int32_t first = civil_to_days(year, r.mon, 1);
  int w1 = (int)((first + 4) % 7);
  if (w1 < 0)
    w1 += 7;
  int day = 1 + (r.dow - w1 + 7) % 7 + (r.week - 1) * 7;
  int dim = days_in_month(year, r.mon);
  while (day > dim)
    day -= 7; // week 5 means "last occurrence"
  return (int64_t)civil_to_days(year, r.mon, day) * 86400 + r.time;
}

int32_t tz_offset_at(const char *posix, time_t utc, bool *isDst) {
  if (isDst)
    *isDst = false;
  if (!posix)
    return 0;
  const char *p = posix;
  const char *e = skip_name(p);
  if (e == p)
    return 0; // no zone name -> unparseable
  p = e;
  int32_t stdPosix;
  if (!parse_hms(p, stdPosix))
    return 0;
  int32_t stdEast = -stdPosix; // POSIX offset is UTC-local; we return east+

  e = skip_name(p);
  if (e == p)
    return stdEast; // no DST designator, fixed offset
  p = e;

  int32_t dstEast = stdEast + 3600; // default: std + 1h
  int32_t dstPosix;
  if (parse_hms(p, dstPosix))
    dstEast = -dstPosix;

  // EU-ish fallback rules when DST name given without explicit rules.
  MRule start = {3, 5, 0, 2 * 3600};
  MRule end = {10, 5, 0, 3 * 3600};
  if (*p == ',') {
    p++;
    if (!parse_mrule(p, start) || *p != ',')
      return 0;
    p++;
    if (!parse_mrule(p, end))
      return 0;
  }

  // Rules are evaluated for the civil year of local standard time.
  int y, mo, d, h, mi, s, wd;
  epoch_to_tm((time_t)((int64_t)utc + stdEast), y, mo, d, h, mi, s, wd);
  // Start rule is expressed in local standard time, end rule in local DST.
  int64_t startUtc = rule_local_secs(y, start) - stdEast;
  int64_t endUtc = rule_local_secs(y, end) - dstEast;
  int64_t u = (int64_t)utc;
  bool dst = (startUtc <= endUtc)
                 ? (u >= startUtc && u < endUtc)
                 : (u >= startUtc || u < endUtc); // southern hemisphere wrap
  if (isDst)
    *isDst = dst;
  return dst ? dstEast : stdEast;
}

// --- Coordinate -> timezone lookup ------------------------------------------
// Bounding boxes scanned in order, first match wins: specific boxes must come
// before the large general ones (UK/Portugal/Finland/Turkey before the CET
// box; Phoenix before Denver; Korea/Japan before China; Darwin/Adelaide
// before the Australian east boxes; Chile before Argentina). Borders are
// deliberately coarse — the UI offers a manual timezone override.

struct TzBox {
  float latMin, latMax, lonMin, lonMax;
  const char *name;
  const char *posix;
};

static const TzBox kZones[] = {
    // Europe
    {63.0f, 67.0f, -25.0f, -13.0f, "Atlantic/Reykjavik", "GMT0"},
    {49.9f, 61.0f, -11.0f, 2.0f, "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {36.8f, 42.2f, -9.6f, -6.2f, "Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {53.0f, 70.1f, 20.5f, 31.6f, "Europe/Helsinki",
     "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {35.8f, 42.2f, 25.9f, 44.8f, "Europe/Istanbul", "<+03>-3"},
    {34.8f, 48.3f, 19.6f, 29.7f, "Europe/Athens",
     "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {36.0f, 71.5f, -9.5f, 24.0f, "Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {43.0f, 70.0f, 31.6f, 60.0f, "Europe/Moscow", "MSK-3"},
    // North America
    {18.9f, 22.3f, -160.3f, -154.8f, "Pacific/Honolulu", "HST10"},
    {51.0f, 71.5f, -170.0f, -129.9f, "America/Anchorage",
     "AKST9AKDT,M3.2.0,M11.1.0"},
    {24.5f, 49.4f, -87.5f, -66.9f, "America/New_York",
     "EST5EDT,M3.2.0,M11.1.0"},
    {25.8f, 49.4f, -102.1f, -87.5f, "America/Chicago",
     "CST6CDT,M3.2.0,M11.1.0"},
    {31.3f, 37.0f, -114.8f, -109.0f, "America/Phoenix", "MST7"},
    {31.3f, 49.4f, -114.1f, -102.1f, "America/Denver",
     "MST7MDT,M3.2.0,M11.1.0"},
    {32.5f, 49.4f, -124.8f, -114.1f, "America/Los_Angeles",
     "PST8PDT,M3.2.0,M11.1.0"},
    // Asia
    {33.1f, 38.7f, 125.9f, 129.6f, "Asia/Seoul", "KST-9"},
    {24.0f, 45.6f, 122.9f, 146.0f, "Asia/Tokyo", "JST-9"},
    {6.7f, 35.7f, 68.1f, 97.4f, "Asia/Kolkata", "IST-5:30"},
    {18.0f, 53.6f, 73.5f, 134.8f, "Asia/Shanghai", "CST-8"},
    // Australia / New Zealand
    {-35.2f, -13.7f, 112.9f, 129.0f, "Australia/Perth", "AWST-8"},
    {-26.0f, -10.9f, 129.0f, 138.0f, "Australia/Darwin", "ACST-9:30"},
    {-38.1f, -26.0f, 129.0f, 141.0f, "Australia/Adelaide",
     "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {-29.0f, -10.7f, 138.0f, 153.6f, "Australia/Brisbane", "AEST-10"},
    {-43.7f, -29.0f, 141.0f, 153.6f, "Australia/Sydney",
     "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {-47.3f, -34.4f, 166.4f, 178.6f, "Pacific/Auckland",
     "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    // South America
    {-56.0f, -17.5f, -75.7f, -66.4f, "America/Santiago",
     "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {-33.8f, -1.0f, -53.0f, -34.8f, "America/Sao_Paulo", "<-03>3"},
    {-55.0f, -21.8f, -73.6f, -53.6f, "America/Argentina", "<-03>3"},
    // Africa
    {-34.9f, -22.1f, 16.4f, 32.9f, "Africa/Johannesburg", "SAST-2"},
    {22.0f, 31.7f, 24.7f, 36.9f, "Africa/Cairo",
     "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {4.0f, 14.0f, -17.5f, 14.6f, "Africa/Lagos", "WAT-1"},
    {-11.7f, 5.0f, 33.9f, 41.9f, "Africa/Nairobi", "EAT-3"},
};

void tz_lookup(float lat, float lon, TzResult &out) {
  for (unsigned i = 0; i < sizeof(kZones) / sizeof(kZones[0]); i++) {
    const TzBox &b = kZones[i];
    if (lat >= b.latMin && lat <= b.latMax && lon >= b.lonMin &&
        lon <= b.lonMax) {
      snprintf(out.name, sizeof(out.name), "%s", b.name);
      snprintf(out.posix, sizeof(out.posix), "%s", b.posix);
      return;
    }
  }
  // Fallback: nearest whole-hour offset from longitude, no DST.
  float q = lon / 15.0f;
  int h = (int)(q >= 0.0f ? q + 0.5f : q - 0.5f);
  if (h < -12)
    h = -12;
  if (h > 14)
    h = 14;
  if (h == 0) {
    snprintf(out.name, sizeof(out.name), "UTC");
    snprintf(out.posix, sizeof(out.posix), "UTC0");
  } else if (h > 0) {
    snprintf(out.name, sizeof(out.name), "UTC+%d", h);
    snprintf(out.posix, sizeof(out.posix), "UTC-%d", h); // POSIX inverted sign
  } else {
    snprintf(out.name, sizeof(out.name), "UTC-%d", -h);
    snprintf(out.posix, sizeof(out.posix), "UTC%d", -h);
  }
}
