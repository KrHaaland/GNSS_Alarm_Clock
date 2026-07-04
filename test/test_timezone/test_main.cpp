// Host-side unit tests for the timezone engine: pio test -e native
#include <string.h>
#include <unity.h>

#include "Timezone.h"
// platformio.ini does not set test_build_src, so the native test build does
// not compile src/. Pull the implementation into this translation unit
// (resolved via -Isrc). If test_build_src is ever enabled, drop this include.
#include "Timezone.cpp"

void setUp(void) {}
void tearDown(void) {}

static time_t mk(int y, int mo, int d, int h, int mi, int s) {
  return tm_to_epoch(y, mo, d, h, mi, s);
}

static const char *OSLO = "CET-1CEST,M3.5.0,M10.5.0/3";
static const char *LONDON = "GMT0BST,M3.5.0/1,M10.5.0";

// --- civil helpers ---

static void test_civil_anchors(void) {
  TEST_ASSERT_EQUAL_INT32(0, civil_to_days(1970, 1, 1));
  TEST_ASSERT_EQUAL_INT32(11017, civil_to_days(2000, 3, 1));
  int y, m, d;
  days_to_civil(0, y, m, d);
  TEST_ASSERT_EQUAL_INT(1970, y);
  TEST_ASSERT_EQUAL_INT(1, m);
  TEST_ASSERT_EQUAL_INT(1, d);
}

static void test_civil_roundtrip(void) {
  // ~1100 years around the epoch, covers leap years and the 2000/2100 rules
  int y, m, d;
  for (int32_t z = -200000; z <= 200000; z++) {
    days_to_civil(z, y, m, d);
    if (civil_to_days(y, m, d) != z) {
      TEST_ASSERT_EQUAL_INT32(z, civil_to_days(y, m, d));
    }
  }
  TEST_PASS();
}

static void test_epoch_known_value(void) {
  TEST_ASSERT_EQUAL_INT64(1768478400LL, (int64_t)mk(2026, 1, 15, 12, 0, 0));
}

static void test_epoch_to_tm_fields_and_wday(void) {
  int y, mo, d, h, mi, s, wd;
  epoch_to_tm(mk(2026, 7, 3, 12, 34, 56), y, mo, d, h, mi, s, wd);
  TEST_ASSERT_EQUAL_INT(2026, y);
  TEST_ASSERT_EQUAL_INT(7, mo);
  TEST_ASSERT_EQUAL_INT(3, d);
  TEST_ASSERT_EQUAL_INT(12, h);
  TEST_ASSERT_EQUAL_INT(34, mi);
  TEST_ASSERT_EQUAL_INT(56, s);
  TEST_ASSERT_EQUAL_INT(5, wd); // 2026-07-03 is a Friday
  epoch_to_tm((time_t)0, y, mo, d, h, mi, s, wd);
  TEST_ASSERT_EQUAL_INT(1970, y);
  TEST_ASSERT_EQUAL_INT(4, wd); // 1970-01-01 was a Thursday
}

// --- POSIX TZ evaluator ---

static void test_oslo_winter(void) {
  bool dst = true;
  TEST_ASSERT_EQUAL_INT32(3600, tz_offset_at(OSLO, mk(2026, 1, 15, 12, 0, 0), &dst));
  TEST_ASSERT_FALSE(dst);
}

static void test_oslo_summer(void) {
  bool dst = false;
  TEST_ASSERT_EQUAL_INT32(7200, tz_offset_at(OSLO, mk(2026, 7, 3, 12, 0, 0), &dst));
  TEST_ASSERT_TRUE(dst);
}

static void test_eu_spring_edge(void) {
  // EU rule M3.5.0 at 02:00 local std = 01:00 UTC; 2026: Sunday March 29
  bool dst = true;
  TEST_ASSERT_EQUAL_INT32(3600, tz_offset_at(OSLO, mk(2026, 3, 29, 0, 59, 59), &dst));
  TEST_ASSERT_FALSE(dst);
  TEST_ASSERT_EQUAL_INT32(7200, tz_offset_at(OSLO, mk(2026, 3, 29, 1, 0, 0), &dst));
  TEST_ASSERT_TRUE(dst);
}

static void test_eu_autumn_edge(void) {
  // M10.5.0/3: 03:00 local DST = 01:00 UTC; 2026: Sunday October 25
  bool dst = false;
  TEST_ASSERT_EQUAL_INT32(7200, tz_offset_at(OSLO, mk(2026, 10, 25, 0, 59, 59), &dst));
  TEST_ASSERT_TRUE(dst);
  TEST_ASSERT_EQUAL_INT32(3600, tz_offset_at(OSLO, mk(2026, 10, 25, 1, 0, 0), &dst));
  TEST_ASSERT_FALSE(dst);
}

static void test_london(void) {
  bool dst;
  TEST_ASSERT_EQUAL_INT32(0, tz_offset_at(LONDON, mk(2026, 1, 15, 12, 0, 0), &dst));
  TEST_ASSERT_FALSE(dst);
  TEST_ASSERT_EQUAL_INT32(3600, tz_offset_at(LONDON, mk(2026, 7, 3, 12, 0, 0), &dst));
  TEST_ASSERT_TRUE(dst);
  // spring rule /1: 01:00 local std = 01:00 UTC
  TEST_ASSERT_EQUAL_INT32(0, tz_offset_at(LONDON, mk(2026, 3, 29, 0, 59, 59), &dst));
  TEST_ASSERT_FALSE(dst);
  TEST_ASSERT_EQUAL_INT32(3600, tz_offset_at(LONDON, mk(2026, 3, 29, 1, 0, 0), &dst));
  TEST_ASSERT_TRUE(dst);
  // autumn default 02:00 local DST = 01:00 UTC
  TEST_ASSERT_EQUAL_INT32(3600, tz_offset_at(LONDON, mk(2026, 10, 25, 0, 59, 59), &dst));
  TEST_ASSERT_TRUE(dst);
  TEST_ASSERT_EQUAL_INT32(0, tz_offset_at(LONDON, mk(2026, 10, 25, 1, 0, 0), &dst));
  TEST_ASSERT_FALSE(dst);
}

static void test_us_eastern(void) {
  const char *tz = "EST5EDT,M3.2.0,M11.1.0";
  bool dst;
  // 2026: 2nd Sunday of March = Mar 8, 02:00 EST = 07:00 UTC
  TEST_ASSERT_EQUAL_INT32(-18000, tz_offset_at(tz, mk(2026, 3, 8, 6, 59, 59), &dst));
  TEST_ASSERT_FALSE(dst);
  TEST_ASSERT_EQUAL_INT32(-14400, tz_offset_at(tz, mk(2026, 3, 8, 7, 0, 0), &dst));
  TEST_ASSERT_TRUE(dst);
  // 1st Sunday of November = Nov 1, 02:00 EDT = 06:00 UTC
  TEST_ASSERT_EQUAL_INT32(-14400, tz_offset_at(tz, mk(2026, 11, 1, 5, 59, 59), &dst));
  TEST_ASSERT_TRUE(dst);
  TEST_ASSERT_EQUAL_INT32(-18000, tz_offset_at(tz, mk(2026, 11, 1, 6, 0, 0), &dst));
  TEST_ASSERT_FALSE(dst);
}

static void test_india(void) {
  bool dst = true;
  TEST_ASSERT_EQUAL_INT32(19800, tz_offset_at("IST-5:30", mk(2026, 7, 3, 0, 0, 0), &dst));
  TEST_ASSERT_FALSE(dst);
  TzResult r;
  tz_lookup(28.6f, 77.2f, r); // Delhi
  TEST_ASSERT_EQUAL_STRING("IST-5:30", r.posix);
  TEST_ASSERT_EQUAL_STRING("Asia/Kolkata", r.name);
}

static void test_sydney(void) {
  const char *tz = "AEST-10AEDT,M10.1.0,M4.1.0/3";
  bool dst;
  TEST_ASSERT_EQUAL_INT32(39600, tz_offset_at(tz, mk(2026, 1, 10, 12, 0, 0), &dst));
  TEST_ASSERT_TRUE(dst); // southern summer
  TEST_ASSERT_EQUAL_INT32(36000, tz_offset_at(tz, mk(2026, 6, 10, 12, 0, 0), &dst));
  TEST_ASSERT_FALSE(dst);
  // spring 2026: 1st Sunday of October = Oct 4, 02:00 AEST = Oct 3 16:00 UTC
  TEST_ASSERT_EQUAL_INT32(36000, tz_offset_at(tz, mk(2026, 10, 3, 15, 59, 59), &dst));
  TEST_ASSERT_FALSE(dst);
  TEST_ASSERT_EQUAL_INT32(39600, tz_offset_at(tz, mk(2026, 10, 3, 16, 0, 0), &dst));
  TEST_ASSERT_TRUE(dst);
}

static void test_angle_bracket_names(void) {
  bool dst = true;
  TEST_ASSERT_EQUAL_INT32(10800, tz_offset_at("<+03>-3", mk(2026, 7, 3, 0, 0, 0), &dst));
  TEST_ASSERT_FALSE(dst);
  TEST_ASSERT_EQUAL_INT32(-10800, tz_offset_at("<-03>3", mk(2026, 7, 3, 0, 0, 0), &dst));
  TEST_ASSERT_FALSE(dst);
  // Chile, /24 rule times: DST in southern summer
  const char *cl = "<-04>4<-03>,M9.1.6/24,M4.1.6/24";
  TEST_ASSERT_EQUAL_INT32(-10800, tz_offset_at(cl, mk(2026, 1, 15, 12, 0, 0), &dst));
  TEST_ASSERT_TRUE(dst);
  TEST_ASSERT_EQUAL_INT32(-14400, tz_offset_at(cl, mk(2026, 6, 15, 12, 0, 0), &dst));
  TEST_ASSERT_FALSE(dst);
}

static void test_unparseable(void) {
  bool dst = true;
  TEST_ASSERT_EQUAL_INT32(0, tz_offset_at("", mk(2026, 1, 1, 0, 0, 0), &dst));
  TEST_ASSERT_FALSE(dst);
  dst = true;
  TEST_ASSERT_EQUAL_INT32(0, tz_offset_at("1234", mk(2026, 1, 1, 0, 0, 0), &dst));
  TEST_ASSERT_FALSE(dst);
  dst = true;
  TEST_ASSERT_EQUAL_INT32(0, tz_offset_at("CET-1CEST,bogus", mk(2026, 1, 1, 0, 0, 0), &dst));
  TEST_ASSERT_FALSE(dst);
}

// --- tz_lookup ---

static void test_lookup_europe(void) {
  TzResult r;
  tz_lookup(59.91f, 10.75f, r); // Oslo
  TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", r.posix);
  TEST_ASSERT_EQUAL_STRING("Europe/Oslo", r.name);
  tz_lookup(60.17f, 24.94f, r); // Helsinki
  TEST_ASSERT_EQUAL_STRING("EET-2EEST,M3.5.0/3,M10.5.0/4", r.posix);
  tz_lookup(51.5f, -0.12f, r); // London
  TEST_ASSERT_EQUAL_STRING("GMT0BST,M3.5.0/1,M10.5.0", r.posix);
}

static void test_lookup_world(void) {
  TzResult r;
  tz_lookup(33.4f, -112.07f, r); // Phoenix (no-DST box before Mountain)
  TEST_ASSERT_EQUAL_STRING("MST7", r.posix);
  tz_lookup(35.7f, 139.7f, r); // Tokyo
  TEST_ASSERT_EQUAL_STRING("JST-9", r.posix);
}

static void test_lookup_fallback(void) {
  TzResult r;
  tz_lookup(0.0f, -30.0f, r); // mid-Atlantic: round(-30/15) = -2
  TEST_ASSERT_EQUAL_STRING("UTC2", r.posix);
  TEST_ASSERT_EQUAL_STRING("UTC-2", r.name);
  bool dst = true;
  TEST_ASSERT_EQUAL_INT32(-7200, tz_offset_at(r.posix, mk(2026, 7, 3, 0, 0, 0), &dst));
  TEST_ASSERT_FALSE(dst);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_civil_anchors);
  RUN_TEST(test_civil_roundtrip);
  RUN_TEST(test_epoch_known_value);
  RUN_TEST(test_epoch_to_tm_fields_and_wday);
  RUN_TEST(test_oslo_winter);
  RUN_TEST(test_oslo_summer);
  RUN_TEST(test_eu_spring_edge);
  RUN_TEST(test_eu_autumn_edge);
  RUN_TEST(test_london);
  RUN_TEST(test_us_eastern);
  RUN_TEST(test_india);
  RUN_TEST(test_sydney);
  RUN_TEST(test_angle_bracket_names);
  RUN_TEST(test_unparseable);
  RUN_TEST(test_lookup_europe);
  RUN_TEST(test_lookup_world);
  RUN_TEST(test_lookup_fallback);
  return UNITY_END();
}
