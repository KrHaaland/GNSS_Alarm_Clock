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
    {36.8f, 39.9f, -31.4f, -24.7f, "Atlantic/Azores", "<-01>1<+00>,M3.5.0/0,M10.5.0/1"},
    {32.3f, 33.2f, -17.4f, -16.2f, "Atlantic/Madeira", "WET0WEST,M3.5.0/1,M10.5.0"},
    {27.5f, 29.5f, -18.3f, -13.3f, "Atlantic/Canary", "WET0WEST,M3.5.0/1,M10.5.0"},
    {61.3f, 62.5f, -7.8f, -6.2f, "Atlantic/Faroe", "WET0WEST,M3.5.0/1,M10.5.0"},
    {54.3f, 55.4f, 19.6f, 22.9f, "Europe/Kaliningrad", "EET-2"},
    {44.3f, 46.3f, 32.4f, 36.7f, "Europe/Simferopol", "MSK-3"},
    {37.6f, 39.8f, 19.3f, 21.0f, "Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {45.5f, 55.0f, 45.5f, 52.0f, "Europe/Samara", "<+04>-4"},
    {54.3f, 55.4f, 19.3f, 22.9f, "Europe/Kaliningrad", "EET-2"},
    {59.5f, 68.5f, 48.0f, 64.0f, "Europe/Moscow", "MSK-3"},
    {46.0f, 55.5f, 45.8f, 54.0f, "Europe/Samara", "<+04>-4"},
    {45.8f, 54.5f, 141.5f, 144.6f, "Asia/Sakhalin", "<+11>-11"},
    {63.0f, 72.0f, -180.0f, -169.0f, "Asia/Anadyr", "<+12>-12"},
    {39.1f, 43.1f, 72.0f, 80.3f, "Asia/Bishkek", "<+06>-6"},
    {38.3f, 41.95f, 44.7f, 50.6f, "Asia/Baku", "<+04>-4"},
    {38.8f, 41.35f, 43.4f, 46.7f, "Asia/Yerevan", "<+04>-4"},
    {41.0f, 43.0f, 40.0f, 46.8f, "Asia/Tbilisi", "<+04>-4"},
    {41.6f, 48.3f, -86.6f, -82.3f, "America/Detroit", "EST5EDT,M3.2.0,M11.1.0"},
    {37.8f, 41.8f, -87.1f, -84.8f, "America/Indiana", "EST5EDT,M3.2.0,M11.1.0"},
    {31.33f, 37.0f, -114.75f, -109.05f, "America/Phoenix", "MST7"},
    {50.5f, 54.5f, -180.0f, -169.5f, "America/Adak", "HST10HDT,M3.2.0,M11.1.0"},
    {51.5f, 53.5f, 172.0f, 180.0f, "America/Adak", "HST10HDT,M3.2.0,M11.1.0"},
    {18.5f, 22.5f, -160.5f, -154.5f, "Pacific/Honolulu", "HST10"},
    {60.0f, 69.7f, -141.2f, -124.0f, "America/Whitehorse", "MST7"},
    {48.9f, 60.0f, -110.0f, -101.4f, "America/Regina", "CST6"},
    {46.5f, 51.8f, -59.5f, -52.5f, "America/St_Johns", "NST3:30NDT,M3.2.0,M11.1.0"},
    {66.0f, 78.0f, -120.0f, -96.0f, "America/Cambridge", "MST7MDT,M3.2.0,M11.1.0"},
    {30.7f, 31.9f, -108.6f, -105.6f, "America/Cd_Juarez", "MST7MDT,M3.2.0,M11.1.0"},
    {29.0f, 30.2f, -105.6f, -103.4f, "America/Ojinaga", "CST6CDT,M3.2.0,M11.1.0"},
    {26.0f, 32.5f, -115.2f, -108.4f, "America/Hermosillo", "MST7"},
    {17.8f, 21.7f, -89.3f, -86.7f, "America/Cancun", "EST5"},
    {25.8f, 29.5f, -101.5f, -97.0f, "America/Matamoros", "CST6CDT,M3.2.0,M11.1.0"},
    {-1.5f, 0.8f, -92.1f, -89.2f, "Pacific/Galapagos", "<-06>6"},
    {-11.2f, -7.0f, -73.5f, -66.7f, "America/Rio_Branco", "<-05>5"},
    {-56.0f, -49.0f, -76.0f, -66.0f, "America/Punta_Aren", "<-03>3"},
    {-27.5f, -27.0f, -109.6f, -109.1f, "Pacific/Easter", "<-06>6<-05>,M9.1.6/22,M4.1.6/22"},
    {-4.0f, -3.7f, -32.6f, -32.3f, "America/Noronha", "<-02>2"},
    {6.7f, 13.7f, 92.1f, 94.3f, "Asia/Kolkata", "IST-5:30"},
    {26.3f, 30.5f, 80.0f, 88.3f, "Asia/Kathmandu", "<+0545>-5:45"},
    {5.8f, 9.9f, 79.5f, 81.9f, "Asia/Colombo", "<+0530>-5:30"},
    {20.5f, 26.7f, 88.5f, 92.7f, "Asia/Dhaka", "<+06>-6"},
    {26.7f, 28.4f, 88.7f, 92.2f, "Asia/Thimphu", "<+06>-6"},
    {9.5f, 28.6f, 92.2f, 97.8f, "Asia/Yangon", "<+0630>-6:30"},
    {45.0f, 52.1f, 87.7f, 95.5f, "Asia/Hovd", "<+07>-7"},
    {1.15f, 1.48f, 103.6f, 104.1f, "Asia/Singapore", "<+08>-8"},
    {4.0f, 5.1f, 114.0f, 115.4f, "Asia/Brunei", "<+08>-8"},
    {22.15f, 22.58f, 113.8f, 114.5f, "Asia/Hong_Kong", "HKT-8"},
    {22.1f, 22.25f, 113.5f, 113.6f, "Asia/Macau", "<+08>-8"},
    {-9.5f, -8.1f, 125.0f, 127.4f, "Asia/Dili", "<+09>-9"},
    {-36.0f, -13.5f, 112.0f, 129.0f, "Australia/Perth", "AWST-8"},
    {-26.0f, -10.5f, 129.0f, 138.0f, "Australia/Darwin", "ACST-9:30"},
    {-28.5f, -9.0f, 138.0f, 154.0f, "Australia/Brisbane", "AEST-10"},
    {-31.9f, -31.4f, 158.8f, 159.2f, "Australia/Lord_Howe", "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0"},
    {-44.5f, -43.5f, -177.0f, -176.0f, "Pacific/Chatham", "<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45"},
    {-29.3f, -28.9f, 167.8f, 168.1f, "Pacific/Norfolk", "<+11>-11<+12>,M10.1.0,M4.1.0/3"},
    {-11.0f, -7.0f, -141.0f, -138.0f, "Pacific/Marquesas", "<-0930>9:30"},
    {-24.0f, -22.0f, -136.0f, -134.0f, "Pacific/Gambier", "<-09>9"},
    {-6.0f, -2.0f, -175.0f, -169.0f, "Pacific/Kanton", "<+13>-13"},
    {-12.0f, 6.0f, -162.0f, -150.0f, "Pacific/Kiritimati", "<+14>-14"},
    {-4.6f, -1.0f, 28.8f, 31.0f, "Africa/Kigali", "CAT-2"},
    {31.0f, 38.4f, 61.5f, 71.1f, "Asia/Kabul", "<+0430>-4:30"},
    {14.5f, 17.5f, -25.5f, -22.5f, "Atlantic/Cape_Verde", "<-01>1"},
    {49.8f, 61.0f, -8.7f, 1.9f, "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {51.3f, 55.5f, -10.6f, -5.9f, "Europe/Dublin", "GMT0BST,M3.5.0/1,M10.5.0"},
    {36.9f, 42.2f, -9.6f, -6.1f, "Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {60.0f, 70.1f, 21.0f, 31.6f, "Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {54.3f, 56.4f, 20.9f, 26.6f, "Europe/Vilnius", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {56.4f, 59.7f, 20.9f, 28.2f, "Europe/Tallinn", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {51.6f, 54.3f, 23.5f, 32.8f, "Europe/Minsk", "<+03>-3"},
    {54.3f, 56.2f, 26.6f, 32.8f, "Europe/Minsk", "<+03>-3"},
    {44.0f, 51.6f, 22.1f, 38.5f, "Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {45.4f, 48.5f, 26.6f, 30.2f, "Europe/Chisinau", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {41.2f, 48.3f, 22.5f, 29.7f, "Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {34.8f, 41.8f, 21.0f, 26.6f, "Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {34.5f, 35.8f, 32.2f, 34.7f, "Asia/Nicosia", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {35.8f, 42.2f, 26.6f, 44.8f, "Europe/Istanbul", "<+03>-3"},
    {52.4f, 60.0f, 28.2f, 50.0f, "Europe/Moscow", "MSK-3"},
    {60.0f, 70.5f, 31.6f, 50.0f, "Europe/Moscow", "MSK-3"},
    {44.0f, 52.4f, 38.5f, 50.0f, "Europe/Moscow", "MSK-3"},
    {41.2f, 70.0f, 27.0f, 52.0f, "Europe/Moscow", "MSK-3"},
    {50.0f, 73.0f, 52.0f, 70.0f, "Asia/Yekaterinburg", "<+05>-5"},
    {54.0f, 59.0f, 70.0f, 77.0f, "Asia/Omsk", "<+06>-6"},
    {51.0f, 79.0f, 77.0f, 99.0f, "Asia/Krasnoyarsk", "<+07>-7"},
    {50.0f, 79.0f, 99.0f, 110.0f, "Asia/Irkutsk", "<+08>-8"},
    {56.0f, 74.0f, 110.0f, 141.0f, "Asia/Yakutsk", "<+09>-9"},
    {49.0f, 56.0f, 110.0f, 130.0f, "Asia/Chita", "<+09>-9"},
    {42.0f, 56.0f, 130.0f, 144.0f, "Asia/Vladivostok", "<+10>-10"},
    {58.0f, 71.0f, 144.0f, 158.0f, "Asia/Magadan", "<+11>-11"},
    {50.0f, 72.0f, 158.0f, 180.0f, "Asia/Kamchatka", "<+12>-12"},
    {37.2f, 45.6f, 55.9f, 73.2f, "Asia/Tashkent", "<+05>-5"},
    {35.1f, 42.8f, 52.4f, 66.7f, "Asia/Ashgabat", "<+05>-5"},
    {36.6f, 41.05f, 67.3f, 75.2f, "Asia/Dushanbe", "<+05>-5"},
    {43.0f, 52.2f, 87.5f, 96.0f, "Asia/Hovd", "<+07>-7"},
    {37.0f, 47.5f, -85.0f, -66.9f, "America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {24.4f, 37.0f, -85.0f, -75.4f, "America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {37.0f, 49.4f, -102.0f, -85.0f, "America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {25.8f, 37.0f, -103.0f, -85.0f, "America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {42.0f, 49.0f, -116.8f, -102.0f, "America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {37.0f, 42.0f, -114.05f, -102.0f, "America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {31.3f, 37.0f, -109.05f, -103.0f, "America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {42.0f, 49.0f, -124.85f, -116.8f, "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {32.4f, 42.0f, -124.5f, -114.05f, "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {51.0f, 71.5f, -169.5f, -129.5f, "America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {48.0f, 60.0f, -139.5f, -120.0f, "America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
    {48.9f, 60.0f, -120.0f, -110.0f, "America/Edmonton", "MST7MDT,M3.2.0,M11.1.0"},
    {60.0f, 78.8f, -124.0f, -102.0f, "America/Yellowknife", "MST7MDT,M3.2.0,M11.1.0"},
    {48.9f, 60.0f, -101.4f, -90.0f, "America/Winnipeg", "CST6CDT,M3.2.0,M11.1.0"},
    {41.6f, 57.0f, -90.0f, -66.5f, "America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},
    {57.0f, 62.5f, -85.0f, -66.5f, "America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},
    {43.0f, 48.3f, -66.5f, -59.5f, "America/Halifax", "AST4ADT,M3.2.0,M11.1.0"},
    {51.3f, 60.5f, -66.5f, -55.4f, "America/Goose_Bay", "AST4ADT,M3.2.0,M11.1.0"},
    {60.0f, 84.0f, -85.0f, -61.0f, "America/Iqaluit", "EST5EDT,M3.2.0,M11.1.0"},
    {60.0f, 78.0f, -102.0f, -85.0f, "America/Rankin", "CST6CDT,M3.2.0,M11.1.0"},
    {28.0f, 31.8f, -108.7f, -103.3f, "America/Chihuahua", "CST6"},
    {22.5f, 28.0f, -115.6f, -105.2f, "America/Mazatlan", "MST7"},
    {21.0f, 23.0f, -105.8f, -104.3f, "America/Mazatlan", "MST7"},
    {28.0f, 32.7f, -118.3f, -112.7f, "America/Tijuana", "PST8PDT,M3.2.0,M11.1.0"},
    {-13.7f, 5.3f, -70.2f, -58.0f, "America/Manaus", "<-04>4"},
    {-23.5f, -7.3f, -61.6f, -50.4f, "America/Cuiaba", "<-04>4"},
    {-25.5f, -18.3f, -70.6f, -67.9f, "America/Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {-37.8f, -25.5f, -73.7f, -69.3f, "America/Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {-49.0f, -37.8f, -75.8f, -71.6f, "America/Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {1.2f, 6.8f, 99.6f, 104.7f, "Asia/Kuala_Lumpur", "<+08>-8"},
    {0.85f, 7.4f, 109.5f, 119.3f, "Asia/Kuching", "<+08>-8"},
    {-11.0f, 6.1f, 95.0f, 114.4f, "Asia/Jakarta", "WIB-7"},
    {-11.0f, 4.9f, 114.4f, 125.0f, "Asia/Makassar", "WITA-8"},
    {-11.0f, 4.7f, 125.0f, 141.1f, "Asia/Jayapura", "WIT-9"},
    {41.5f, 52.2f, 95.5f, 119.9f, "Asia/Ulaanbaatar", "<+08>-8"},
    {33.0f, 43.0f, 124.6f, 131.2f, "Asia/Seoul", "KST-9"},
    {21.9f, 25.4f, 119.3f, 122.1f, "Asia/Taipei", "CST-8"},
    {24.0f, 30.0f, 122.85f, 129.7f, "Asia/Tokyo", "JST-9"},
    {-38.5f, -26.0f, 129.0f, 141.0f, "Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {-44.0f, -28.5f, 141.0f, 154.0f, "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {4.0f, 8.0f, 154.5f, 163.0f, "Pacific/Pohnpei", "<+11>-11"},
    {0.0f, 11.0f, 137.0f, 154.5f, "Pacific/Chuuk", "<+10>-10"},
    {23.5f, 37.5f, 61.0f, 74.6f, "Asia/Karachi", "PKT-5"},
    {59.0f, 84.0f, -73.0f, -40.0f, "America/Nuuk", "<-02>2<-01>,M3.5.0,M10.5.0/0"},
    {63.0f, 66.8f, -24.7f, -13.2f, "Atlantic/Reykjavik", "GMT0"},
    {35.9f, 51.1f, -9.5f, 8.3f, "Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {54.5f, 71.5f, 4.5f, 23.5f, "Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {45.7f, 55.1f, 2.5f, 24.2f, "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {35.3f, 47.1f, 6.5f, 22.5f, "Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {40.5f, 54.9f, 46.5f, 87.5f, "Asia/Almaty", "<+05>-5"},
    {41.5f, 52.2f, 96.0f, 120.5f, "Asia/Ulaanbaatar", "<+08>-8"},
    {14.3f, 30.0f, -105.7f, -86.5f, "America/Mexico_City", "CST6"},
    {7.9f, 18.5f, -92.5f, -82.9f, "America/Guatemala", "CST6"},
    {7.0f, 9.7f, -82.9f, -77.1f, "America/Panama", "EST5"},
    {-4.3f, 13.0f, -79.1f, -72.0f, "America/Bogota", "<-05>5"},
    {-5.0f, 1.5f, -81.1f, -75.2f, "America/Guayaquil", "<-05>5"},
    {-18.4f, 0.05f, -81.4f, -69.0f, "America/Lima", "<-05>5"},
    {0.6f, 12.6f, -72.0f, -59.8f, "America/Caracas", "<-04>4"},
    {-22.2f, -9.7f, -69.0f, -57.5f, "America/La_Paz", "<-04>4"},
    {1.1f, 8.6f, -61.4f, -57.2f, "America/Guyana", "<-04>4"},
    {1.8f, 6.1f, -57.2f, -53.9f, "America/Paramaribo", "<-03>3"},
    {2.1f, 5.8f, -54.6f, -51.6f, "America/Cayenne", "<-03>3"},
    {-34.0f, 5.3f, -58.0f, -34.7f, "America/Sao_Paulo", "<-03>3"},
    {-55.1f, -22.2f, -73.6f, -53.6f, "America/Cordoba", "<-03>3"},
    {-27.6f, -19.3f, -62.6f, -54.3f, "America/Asuncion", "<-03>3"},
    {-35.0f, -30.0f, -58.5f, -53.0f, "America/Montevideo", "<-03>3"},
    {19.7f, 23.3f, -85.0f, -74.0f, "America/Havana", "CST5CDT,M3.2.0/0,M11.1.0/1"},
    {18.0f, 20.1f, -74.5f, -71.7f, "America/Port-au-Pr", "EST5EDT,M3.2.0,M11.1.0"},
    {17.5f, 20.1f, -71.7f, -64.2f, "America/Puerto_Rico", "AST4"},
    {17.6f, 18.6f, -78.5f, -76.1f, "America/Jamaica", "EST5"},
    {6.7f, 35.7f, 68.1f, 97.5f, "Asia/Kolkata", "IST-5:30"},
    {-0.8f, 7.1f, 72.5f, 73.8f, "Asia/Maldives", "<+05>-5"},
    {5.5f, 22.4f, 97.5f, 109.6f, "Asia/Bangkok", "<+07>-7"},
    {29.0f, 38.0f, 128.5f, 136.0f, "Asia/Tokyo", "JST-9"},
    {34.0f, 45.6f, 136.0f, 146.0f, "Asia/Tokyo", "JST-9"},
    {4.5f, 21.2f, 116.9f, 126.7f, "Asia/Manila", "<+08>-8"},
    {18.1f, 53.6f, 73.5f, 134.8f, "Asia/Shanghai", "CST-8"},
    {-47.5f, -34.0f, 166.0f, 179.0f, "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {-23.0f, -19.0f, 163.0f, 168.5f, "Pacific/Noumea", "<+11>-11"},
    {-20.5f, -13.0f, 166.0f, 170.5f, "Pacific/Efate", "<+11>-11"},
    {-12.5f, -3.5f, 154.0f, 167.0f, "Pacific/Guadalcanal", "<+11>-11"},
    {-21.0f, -12.0f, 176.0f, 180.0f, "Pacific/Fiji", "<+12>-12"},
    {-11.0f, -5.0f, 176.0f, 180.0f, "Pacific/Funafuti", "<+12>-12"},
    {-14.5f, -13.0f, -179.0f, -176.0f, "Pacific/Wallis", "<+12>-12"},
    {-24.0f, -15.0f, -177.0f, -173.0f, "Pacific/Tongatapu", "<+13>-13"},
    {-14.2f, -13.0f, -173.0f, -171.4f, "Pacific/Apia", "<+13>-13"},
    {-9.5f, -8.4f, -172.7f, -171.0f, "Pacific/Fakaofo", "<+13>-13"},
    {-14.5f, -14.0f, -171.3f, -169.5f, "Pacific/Pago_Pago", "SST11"},
    {-19.5f, -18.5f, -170.2f, -169.5f, "Pacific/Niue", "<-11>11"},
    {-22.0f, -18.0f, -160.0f, -157.0f, "Pacific/Rarotonga", "<-10>10"},
    {-28.0f, -7.0f, -154.0f, -134.0f, "Pacific/Tahiti", "<-10>10"},
    {-3.0f, 4.0f, 172.0f, 177.0f, "Pacific/Tarawa", "<+12>-12"},
    {-1.0f, 0.0f, 166.0f, 167.5f, "Pacific/Nauru", "<+12>-12"},
    {4.0f, 15.0f, 165.0f, 173.0f, "Pacific/Majuro", "<+12>-12"},
    {2.0f, 11.0f, 131.0f, 135.0f, "Pacific/Palau", "<+09>-9"},
    {13.0f, 21.0f, 144.0f, 146.5f, "Pacific/Guam", "ChST-10"},
    {-26.0f, -23.0f, -131.0f, -124.0f, "Pacific/Pitcairn", "<-08>8"},
    {-12.0f, -1.0f, 140.0f, 154.0f, "Port_Moresby", "<+10>-10"},
    {21.0f, 36.1f, -17.2f, -1.0f, "Africa/Casablanca", "<+01>-1"},
    {18.5f, 37.5f, -1.0f, 11.7f, "Africa/Algiers", "CET-1"},
    {4.0f, 27.0f, -17.6f, 2.0f, "Africa/Abidjan", "GMT0"},
    {-18.0f, 23.5f, 2.0f, 24.0f, "Africa/Lagos", "WAT-1"},
    {19.5f, 33.5f, 9.0f, 25.5f, "Africa/Tripoli", "EET-2"},
    {21.7f, 31.9f, 24.6f, 34.9f, "Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {3.4f, 22.3f, 21.8f, 37.5f, "Africa/Khartoum", "CAT-2"},
    {-29.0f, -16.9f, 11.5f, 25.3f, "Africa/Windhoek", "CAT-2"},
    {-27.0f, -8.0f, 30.0f, 41.0f, "Africa/Maputo", "CAT-2"},
    {-35.0f, -22.0f, 16.0f, 33.5f, "Africa/Johannesburg", "SAST-2"},
    {-5.0f, 18.2f, 35.0f, 52.0f, "Africa/Nairobi", "EAT-3"},
    {-11.8f, 4.5f, 29.3f, 41.0f, "Africa/Kampala", "EAT-3"},
    {-26.0f, -11.5f, 43.0f, 50.9f, "Indian/Antananarivo", "EAT-3"},
    {-21.5f, -3.5f, 54.5f, 63.6f, "Indian/Mauritius", "<+04>-4"},
    {29.4f, 33.4f, 34.2f, 35.7f, "Asia/Jerusalem", "IST-2IDT,M3.5.5/2,M10.5.0/2"},
    {33.0f, 34.7f, 35.0f, 36.7f, "Asia/Beirut", "EET-2EEST,M3.5.0/0,M10.5.0/0"},
    {29.0f, 37.4f, 35.0f, 42.5f, "Asia/Amman", "<+03>-3"},
    {29.0f, 37.4f, 38.8f, 48.0f, "Asia/Baghdad", "<+03>-3"},
    {12.0f, 32.2f, 36.0f, 52.0f, "Asia/Riyadh", "<+03>-3"},
    {16.0f, 26.5f, 51.8f, 60.0f, "Asia/Dubai", "<+04>-4"},
    {26.3f, 40.0f, 48.6f, 61.0f, "Asia/Tehran", "<+0330>-3:30"},
    {32.5f, 40.0f, 44.0f, 48.6f, "Asia/Tehran", "<+0330>-3:30"},
};

// --- Polygon zones -----------------------------------------------------------
// Checked before the box table: polygons drawn with tools/tz_polygon_editor.html
// capture borders the axis-aligned boxes can't (fjords, panhandles, enclaves).
// Vertices are centi-degrees in int16 (0.01 deg ~ 1.1 km, 4 bytes/vertex).

struct TzPolyZone {
  int16_t latMin, latMax, lonMin, lonMax; // bounding box prefilter, centi-deg
  uint16_t first, count;                  // vertex-PAIR range in kPolyVerts
  const char *name;
  const char *posix;
};

#include "TimezonePolyData.h"

static int32_t to_cdeg(float deg) {
  return (int32_t)(deg * 100.0f + (deg >= 0.0f ? 0.5f : -0.5f));
}

// Even-odd ray casting (PNPOLY), integer-only: `v` is [lat,lon] pairs.
// The division in the classic crossing test is replaced by a cross product
// whose sign is interpreted per edge direction.
static bool point_in_poly(int32_t lat, int32_t lon, const int16_t *v,
                          uint16_t n) {
  bool inside = false;
  for (uint16_t i = 0, j = n - 1; i < n; j = i++) {
    int32_t latI = v[2 * i], lonI = v[2 * i + 1];
    int32_t latJ = v[2 * j], lonJ = v[2 * j + 1];
    if ((latI > lat) == (latJ > lat))
      continue; // edge doesn't straddle the ray's latitude
    int64_t s = (int64_t)(lonJ - lonI) * (lat - latI) -
                (int64_t)(lon - lonI) * (latJ - latI);
    if (latJ > latI ? s > 0 : s < 0)
      inside = !inside;
  }
  return inside;
}

bool tz_lookup_poly(float lat, float lon, TzResult &out) {
  int32_t la = to_cdeg(lat), lo = to_cdeg(lon);
  for (unsigned i = 0; i < kPolyZoneCount; i++) {
    const TzPolyZone &z = kPolyZones[i];
    if (la < z.latMin || la > z.latMax || lo < z.lonMin || lo > z.lonMax)
      continue;
    if (point_in_poly(la, lo, &kPolyVerts[2u * z.first], z.count)) {
      snprintf(out.name, sizeof(out.name), "%s", z.name);
      snprintf(out.posix, sizeof(out.posix), "%s", z.posix);
      return true;
    }
  }
  return false;
}

void tz_lookup(float lat, float lon, TzResult &out) {
  if (tz_lookup_poly(lat, lon, out))
    return;
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
