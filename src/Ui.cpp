// Ui.cpp — LVGL 9.5 interface for the 284x76 ST7789 color TFT. See Ui.h.
//
// Input model (4 keys, no touch): B2/B3 move focus (LV_KEY_PREV/NEXT),
// B4 = ENTER, B1 = back. LVGL's keypad indev reserves PREV/NEXT for focus
// moves, so value widgets (roller/dropdown/slider/btnmatrix) use a local
// "edit mode": B4 on such a widget enters it, then B2/B3 adjust the value
// (LV_KEY_LEFT/RIGHT), B4 commits / B1 cancels.
#include "Ui.h"

#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "Settings.h"
#include "ClockKeeper.h"
#include "Gnss.h"
#include "AlarmManager.h"
#include "TuneStorage.h"
#include "AudioEngine.h"
#include "AmpTPA2016.h"
#include "Leds.h"
#include "PmicNPM1300.h"
#include "Display.h"
#include "RtcRV3028.h"
#include "Timezone.h"
#include "TzTable.h"

#define UI_FW_VERSION "v1.0.0"
#define UI_MAX_WAVS 12
#define UI_REFRESH_MS 250
#define UI_SAVE_DEBOUNCE_MS 1500
#define UI_TEST_PREVIEW_MS 5000

// 100 px Montserrat subset for the big clock figure (src/font_clock_100.c)
LV_FONT_DECLARE(font_clock_100)

// ---------------------------------------------------------------- state ---
enum UiScreen : uint8_t {
  SCR_CLOCK, SCR_MENU, SCR_ALARM, SCR_TZ, SCR_DISP, SCR_TUNES, SCR_SYS,
  SCR_SKY, SCR_PMIC, SCR_RING
};
#define SCR_COUNT 10

static UiScreen s_screen = SCR_CLOCK;
static lv_group_t *s_group;
static lv_indev_t *s_indev;

static bool s_editing;         // local edit mode for value widgets
static uint16_t s_rollerOrig;  // roller value on edit-enter (for commit/cancel)
static bool s_dimmed;
static uint32_t s_lastActMs;
static uint32_t s_lastRefreshMs;

static bool s_dirty;           // deferred settings_save (spare flash cycles)
static uint32_t s_dirtyMs;

static bool s_previewing;
static uint32_t s_previewDeadline; // 0 = no timeout (Tunes screen)

static lv_obj_t *s_msgbox;

// ------------------------------------------------------------- widgets ---
static lv_obj_t *s_scr[SCR_COUNT]; // indexed by UiScreen

// Clock
static lv_obj_t *s_ckStatL, *s_ckStatR;
static lv_obj_t *s_ckBig, *s_ckSec, *s_ckAmpm, *s_ckBottom;
static char s_cStatL[24], s_cStatR[24];
static char s_cBig[12], s_cSec[8], s_cAmpm[8], s_cBottom[72];

// Starry night: white dots behind the clock, shown 22:00-06:00 when enabled.
// Created before the labels so they stay in the background; label glyphs
// draw over them (their backgrounds are transparent).
#define STAR_COUNT 26
static lv_obj_t *s_stars[STAR_COUNT];
static bool s_starsShown;
static uint32_t s_twinkleRnd = 0x1234567u;
static const uint8_t STAR_POS[STAR_COUNT][2] = {
    // x/2 (0..213), y — spread by hand so no rows/diagonals form
    {6, 18},    {22, 120}, {35, 35},  {49, 88},  {65, 14},  {75, 110},
    {87, 52},   {100, 128},{107, 8},  {120, 70}, {131, 30}, {142, 115},
    {150, 55},  {161, 10}, {170, 90}, {180, 40}, {192, 125},{200, 20},
    {207, 75},  {15, 70},  {30, 132}, {55, 60},  {95, 95},  {125, 105},
    {155, 130}, {210, 110}};

static void stars_set(bool on) {
  if (on == s_starsShown)
    return;
  s_starsShown = on;
  for (uint8_t i = 0; i < STAR_COUNT; i++) {
    if (on)
      lv_obj_remove_flag(s_stars[i], LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_stars[i], LV_OBJ_FLAG_HIDDEN);
  }
}

static void stars_twinkle() { // one star flips brightness per UI tick
  s_twinkleRnd = s_twinkleRnd * 1664525u + 1013904223u;
  lv_obj_t *st = s_stars[(s_twinkleRnd >> 16) % STAR_COUNT];
  lv_obj_set_style_bg_opa(st, ((s_twinkleRnd >> 8) & 1) ? LV_OPA_COVER
                                                        : LV_OPA_40, 0);
}

// Menu
static lv_obj_t *s_fMenu[11];
static lv_obj_t *s_tapLabel;  // "Tap snooze: On/Off" toggle item label
static lv_obj_t *s_modeLabel; // "Mode: ..." cycle item label

// AlarmEdit
static int8_t s_alarmIdx;
static lv_obj_t *s_alTitle, *s_alEnable, *s_alHour, *s_alMin, *s_alDays;
static lv_obj_t *s_alTune, *s_alRamp, *s_alJit, *s_alTest, *s_alSave;
static lv_obj_t *s_fAlarm[9];

// Time & zone
static lv_obj_t *s_tzAuto, *s_tzZone, *s_tz24, *s_tzSync, *s_tzInfo;
static lv_obj_t *s_fTz[4];
static char s_cTzInfo[64];
static uint32_t s_tzNoteUntil;

// Display
static lv_obj_t *s_dVolume, *s_dBright, *s_dDim, *s_dDimLvl, *s_dStarry;
static lv_obj_t *s_fDisp[5];

// Tunes
static lv_obj_t *s_tuneList;
static lv_obj_t *s_fTunes[AUDIO_MELODY_COUNT + UI_MAX_WAVS];
static uint8_t s_nfTunes;

// SysInfo
static lv_obj_t *s_sysLabel;
static char s_cSys[400];

// Battery / PMIC (v2 boards)
static lv_obj_t *s_pmicLabel;
static char s_cPmic[220];

// Sky view (GSV): polar az/elev plot + SNR bars for the strongest sats
#define SKY_MAX_SATS 24
#define SKY_MAX_BARS 12
static lv_obj_t *s_skyDots[SKY_MAX_SATS];
static lv_obj_t *s_skyBars[SKY_MAX_BARS], *s_skyBarLbl[SKY_MAX_BARS];
static lv_obj_t *s_skyInfo;
static GnssSatInfo s_skyLast[SKY_MAX_SATS];
static uint8_t s_skyLastN;

// Ringing
static lv_obj_t *s_rgTitle, *s_rgTime, *s_rgHint;

// ------------------------------------------------------- static tables ---
static const char *const DAY_ABBR[7] = {"Sun", "Mon", "Tue", "Wed",
                                        "Thu", "Fri", "Sat"};
static const char *const MON_ABBR[12] = {"Jan", "Feb", "Mar", "Apr",
                                         "May", "Jun", "Jul", "Aug",
                                         "Sep", "Oct", "Nov", "Dec"};

static const char *DAYS_MAP[8] = {"S", "M", "T", "W", "T", "F", "S", ""};


static const char DIM_OPTS[] = "Never\n15 s\n30 s\n1 min\n5 min";
static const char RAMP_OPTS[] = "Off\n15 s\n30 s\n60 s";
static const uint8_t RAMP_SECONDS[4] = {0, 15, 30, 60};
// Random trigger offset (the +- glyph is outside the ASCII font range)
static const char JITTER_OPTS[] = "Off\n+/-1 min\n+/-5 min\n+/-9 min";
static const uint8_t JITTER_MINUTES[4] = {0, 1, 5, 9};
static const uint16_t DIM_SECONDS[5] = {0, 15, 30, 60, 300};

// Roller option strings, built once in ui_begin (no heap).
static char s_hourOpts[24 * 3];
static char s_minOpts[60 * 3];

// Tune catalog: builtin melodies first, then WAVs from the flash drive.
static char s_tuneNames[UI_MAX_WAVS][32];
static uint8_t s_tuneCount;
static char s_tuneOpts[AUDIO_MELODY_COUNT * 16 + UI_MAX_WAVS * 33];

// ------------------------------------------------------------ key queue ---
static uint32_t s_keyq[8];
static uint8_t s_kqHead, s_kqTail;
static uint32_t s_curKey;
static bool s_curPressed;

static void push_key(uint32_t key) {
  uint8_t nt = (uint8_t)((s_kqTail + 1) & 7);
  if (nt == s_kqHead)
    return; // full: drop
  s_keyq[s_kqTail] = key;
  s_kqTail = nt;
}

// One PRESSED read per queued key, then a RELEASED read.
static void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  if (s_curPressed) {
    data->key = s_curKey;
    data->state = LV_INDEV_STATE_RELEASED;
    s_curPressed = false;
  } else if (s_kqHead != s_kqTail) {
    s_curKey = s_keyq[s_kqHead];
    s_kqHead = (uint8_t)((s_kqHead + 1) & 7);
    s_curPressed = true;
    data->key = s_curKey;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->key = s_curKey;
    data->state = LV_INDEV_STATE_RELEASED;
  }
  data->continue_reading = s_curPressed || (s_kqHead != s_kqTail);
}

// -------------------------------------------------------------- helpers ---
static void set_label_if(lv_obj_t *lbl, char *cache, size_t cap,
                         const char *txt, bool force) {
  if (!force && strcmp(cache, txt) == 0)
    return;
  snprintf(cache, cap, "%s", txt);
  lv_label_set_text(lbl, txt);
}

static bool nocase_eq(const char *a, const char *b) {
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    if (ca != cb)
      return false;
    ++a; ++b;
  }
  return *a == *b;
}

static void fmt_hm(char *out, size_t n, uint8_t h, uint8_t m) {
  if (settings().use24h) {
    snprintf(out, n, "%02u:%02u", h, m);
  } else {
    uint8_t d = h % 12;
    if (d == 0)
      d = 12;
    snprintf(out, n, "%u:%02u %s", d, m, h < 12 ? "AM" : "PM");
  }
}

static void fmt_coord(char *out, size_t n, float v) {
  long s = lroundf(v * 10000.0f);
  const char *sg = "";
  if (s < 0) {
    sg = "-";
    s = -s;
  }
  snprintf(out, n, "%s%ld.%04ld", sg, s / 10000, s % 10000);
}

static void mark_dirty() {
  s_dirty = true;
  s_dirtyMs = millis();
}

static bool is_editable(lv_obj_t *o) {
  return o && (lv_obj_check_type(o, &lv_roller_class) ||
               lv_obj_check_type(o, &lv_dropdown_class) ||
               lv_obj_check_type(o, &lv_slider_class) ||
               lv_obj_check_type(o, &lv_buttonmatrix_class));
}

static void force_exit_edit() {
  lv_obj_t *f = s_group ? lv_group_get_focused(s_group) : NULL;
  if (f) {
    lv_obj_remove_state(f, LV_STATE_EDITED);
    if (lv_obj_check_type(f, &lv_dropdown_class))
      lv_dropdown_close(f);
  }
  s_editing = false;
}

static void slider_step(lv_obj_t *s, int step) {
  int v = lv_slider_get_value(s) + step;
  int mn = lv_slider_get_min_value(s);
  int mx = lv_slider_get_max_value(s);
  if (v < mn) v = mn;
  if (v > mx) v = mx;
  if (v != lv_slider_get_value(s)) {
    lv_slider_set_value(s, v, LV_ANIM_OFF);
    lv_obj_send_event(s, LV_EVENT_VALUE_CHANGED, NULL);
  }
}

// Step a roller directly (±1). More reliable than pushing LV_KEY through the
// indev, and lv_roller_set_selected also commits the value (updates the
// shadow the roller would otherwise revert to on defocus).
static void roller_step(lv_obj_t *r, int delta) {
  int32_t cnt = (int32_t)lv_roller_get_option_count(r);
  int32_t v = (int32_t)lv_roller_get_selected(r) + delta;
  if (v < 0) v = 0;
  if (v >= cnt) v = cnt - 1;
  lv_roller_set_selected(r, (uint32_t)v, LV_ANIM_ON);
}

static void blacken(lv_obj_t *o); // true-black bg; defined near ui_begin

static void show_msgbox(const char *title, const char *txt) {
  if (s_msgbox)
    return;
  s_msgbox = lv_msgbox_create(NULL);
  if (title && title[0])
    lv_msgbox_add_title(s_msgbox, title);
  lv_msgbox_add_text(s_msgbox, txt);
  lv_msgbox_add_close_button(s_msgbox);
  lv_obj_set_width(s_msgbox, 210);
  lv_obj_set_style_text_font(s_msgbox, &lv_font_montserrat_16, 0);
  lv_obj_center(s_msgbox);
  blacken(s_msgbox); // modal is created dynamically -> force black too
}

static void close_msgbox() {
  if (!s_msgbox)
    return;
  lv_msgbox_close(s_msgbox);
  s_msgbox = NULL;
}

// ------------------------------------------------------------- preview ---
static void preview_stop() {
  if (!s_previewing)
    return;
  s_previewing = false;
  audio_stop();
  amp_enable(false);
}

// sel: 0..AUDIO_MELODY_COUNT-1 = builtin, >= AUDIO_MELODY_COUNT = WAV index
static void preview_start(uint16_t sel, uint32_t timeoutMs) {
  bool isWav = sel >= AUDIO_MELODY_COUNT;
  if (isWav && storage_busy()) {
    show_msgbox("USB busy", "Eject the USB drive to preview WAV files.");
    return;
  }
  preview_stop();
  amp_set_volume(settings().volume);
  audio_set_volume(10); // digital pinned; the amp limiter sets loudness
  amp_enable(true);
  bool ok = false;
  if (isWav && (sel - AUDIO_MELODY_COUNT) < s_tuneCount)
    ok = audio_play_wav(s_tuneNames[sel - AUDIO_MELODY_COUNT], true);
  if (!ok)
    audio_play_melody(isWav ? 0 : (uint8_t)sel, true);
  s_previewing = true;
  s_previewDeadline = timeoutMs ? millis() + timeoutMs : 0;
}

// -------------------------------------------------------- tune catalog ---
static void tunes_scan() {
  s_tuneCount = 0;
  if (storage_mounted() && !storage_busy())
    s_tuneCount = storage_list_tunes(s_tuneNames, UI_MAX_WAVS);
  char *p = s_tuneOpts;
  size_t left = sizeof(s_tuneOpts);
  for (uint8_t i = 0; i < AUDIO_MELODY_COUNT; i++) {
    int n = snprintf(p, left, "%s%s", i ? "\n" : "", AUDIO_MELODY_NAMES[i]);
    p += n; left -= n;
  }
  for (uint8_t i = 0; i < s_tuneCount && left > 34; i++) {
    int n = snprintf(p, left, "\n%s", s_tuneNames[i]);
    p += n; left -= n;
  }
}

// -------------------------------------------------- screen infrastructure ---
static void load_screen(UiScreen id); // fwd

static void group_set(lv_obj_t *const *objs, uint8_t n) {
  lv_group_remove_all_objs(s_group);
  for (uint8_t i = 0; i < n; i++)
    if (objs[i])
      lv_group_add_obj(s_group, objs[i]);
  if (n && objs[0])
    lv_group_focus_obj(objs[0]);
}

// Transparent flex row: "Label ........ widget"
static lv_obj_t *make_row(lv_obj_t *parent, const char *name) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  if (name) {
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text_static(lbl, name);
  }
  return row;
}

static lv_obj_t *make_settings_screen() {
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(scr, 6, 0);
  lv_obj_set_style_pad_row(scr, 6, 0);
  return scr;
}

// ------------------------------------------------------------ Clock scr ---
static void refresh_clock(bool force) {
  char b[72];

  if (gnss_has_fix())
    snprintf(b, sizeof(b), LV_SYMBOL_GPS " %u", (unsigned)gnss_num_sats());
  else
    snprintf(b, sizeof(b), LV_SYMBOL_GPS " --");
  set_label_if(s_ckStatL, s_cStatL, sizeof(s_cStatL), b, force);

  // (time source + DST indicator removed from the clock face; still shown on
  //  the System info screen.)

  b[0] = '\0';
  if (storage_busy())
    strcat(b, LV_SYMBOL_USB);
  if (supercaps_ready())
    strcat(b, LV_SYMBOL_BATTERY_FULL);
  for (uint8_t i = 0; i < NUM_ALARMS; i++)
    if (settings().alarms[i].enabled) {
      strcat(b, LV_SYMBOL_BELL);
      break;
    }
  set_label_if(s_ckStatR, s_cStatR, sizeof(s_cStatR), b, force);

  // Big figure + small side label per mode. Modes other than the clock render
  // first and fall through only for the bottom (date/alarm) line.
  const uint8_t mode = settings().mode;
  if (mode != MODE_CLOCK) {
    char big[12];
    const char *side = "";
    if (mode == MODE_SPEED || mode == MODE_ALT) {
      side = (mode == MODE_SPEED) ? "km/h" : "m";
      float v;
      bool ok = (mode == MODE_SPEED) ? gnss_get_speed_kmph(v)
                                     : gnss_get_altitude_m(v);
      if (!ok) {
        strcpy(big, "---"); // no fix (yet)
      } else if (mode == MODE_SPEED) {
        unsigned s10 = (unsigned)(v * 10.0f + 0.5f);
        if (s10 >= 1000) // >=100 km/h: drop the decimal, keep it narrow
          snprintf(big, sizeof(big), "%u", s10 / 10u);
        else
          snprintf(big, sizeof(big), "%u.%u", s10 / 10u, s10 % 10u);
      } else {
        int m = (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
        snprintf(big, sizeof(big), "%d", m);
      }
    } else { // MODE_GAME: UsbGamepad streams HID reports; just say so
      strcpy(big, "GAME");
    }
    bool bigChanged = force || strcmp(s_cBig, big) != 0;
    set_label_if(s_ckBig, s_cBig, sizeof(s_cBig), big, force);
    if (bigChanged) {
      lv_obj_update_layout(s_ckBig);
      // Sit the tag on the digits' baseline: the 100 px font carries 26 px
      // of descent below it, the 20 px tag font ~5 px.
      lv_obj_align_to(s_ckAmpm, s_ckBig, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -21);
    }
    set_label_if(s_ckAmpm, s_cAmpm, sizeof(s_cAmpm), side, force);
    stars_set(false); // stars belong to the clock face only

    if (mode == MODE_GAME) {
      set_label_if(s_ckBottom, s_cBottom, sizeof(s_cBottom),
                   "tilt=stick  B2-B4=btns  B1=menu", force);
      return;
    }
  }

  if (!clock_valid()) {
    stars_set(false);
    if (mode == MODE_CLOCK) {
      set_label_if(s_ckBig, s_cBig, sizeof(s_cBig), "--:--", force);
      set_label_if(s_ckSec, s_cSec, sizeof(s_cSec), "", force);
      set_label_if(s_ckAmpm, s_cAmpm, sizeof(s_cAmpm), "", force);
    }
    set_label_if(s_ckBottom, s_cBottom, sizeof(s_cBottom),
                 "waiting for time...", force);
    return;
  }

  time_t lt = clock_now_local();
  int y, mo, d, h, mi, se, wd;
  epoch_to_tm(lt, y, mo, d, h, mi, se, wd);

  if (mode == MODE_CLOCK) {
    int dh = h;
    const char *ampm = "";
    if (!settings().use24h) {
      ampm = (h < 12) ? "AM" : "PM";
      dh = h % 12;
      if (dh == 0)
        dh = 12;
    }
    snprintf(b, sizeof(b), settings().use24h ? "%02d:%02d" : "%d:%02d", dh, mi);
    bool bigChanged = force || strcmp(s_cBig, b) != 0;
    set_label_if(s_ckBig, s_cBig, sizeof(s_cBig), b, force);
    if (bigChanged) { // width may change -> re-anchor the AM/PM tag
      lv_obj_update_layout(s_ckBig);
      // Sit the tag on the digits' baseline: the 100 px font carries 26 px
      // of descent below it, the 20 px tag font ~5 px.
      lv_obj_align_to(s_ckAmpm, s_ckBig, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -21);
    }
    set_label_if(s_ckAmpm, s_cAmpm, sizeof(s_cAmpm), ampm, force);

    // Starry night: stars behind the digits between 22:00 and 06:00
    stars_set(settings().starryNight && (h >= 22 || h < 6));
    if (s_starsShown)
      stars_twinkle();
  }
  (void)se; // seconds are intentionally not displayed

  int n = snprintf(b, sizeof(b), "%s %02d %s %04d", DAY_ABBR[wd], d,
                   MON_ABBR[(mo - 1) % 12], y);
  time_t nextL;
  int8_t ai;
  if (alarm_next_occurrence(lt, nextL, ai)) {
    int ny, nmo, nd, nh, nmi, nse, nwd;
    epoch_to_tm(nextL, ny, nmo, nd, nh, nmi, nse, nwd);
    char hm[12];
    fmt_hm(hm, sizeof(hm), (uint8_t)nh, (uint8_t)nmi);
    bool today = (nextL / 86400) == (lt / 86400);
    snprintf(b + n, sizeof(b) - n, "  |  " LV_SYMBOL_BELL " %s %s",
             today ? "Today" : DAY_ABBR[nwd], hm);
  }
  set_label_if(s_ckBottom, s_cBottom, sizeof(s_cBottom), b, force);
}

static void make_clock() {
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  s_scr[SCR_CLOCK] = scr;

  // Stars first: creation order = z-order, labels must draw on top.
  for (uint8_t i = 0; i < STAR_COUNT; i++) {
    lv_obj_t *st = lv_obj_create(scr);
    uint8_t d = (i % 5 == 0) ? 3 : 2;
    lv_obj_set_size(st, d, d);
    lv_obj_set_pos(st, (int32_t)STAR_POS[i][0] * 2, STAR_POS[i][1]);
    lv_obj_set_style_radius(st, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(st, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(st, (i % 3 == 0)   ? LV_OPA_COVER
                                : (i % 3 == 1) ? LV_OPA_70
                                               : LV_OPA_40, 0);
    lv_obj_set_style_border_width(st, 0, 0);
    lv_obj_remove_flag(st, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                           LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_flag(st, LV_OBJ_FLAG_HIDDEN);
    s_stars[i] = st;
  }

  s_ckStatL = lv_label_create(scr);
  lv_obj_set_style_text_font(s_ckStatL, &lv_font_montserrat_16, 0);
  lv_obj_align(s_ckStatL, LV_ALIGN_TOP_LEFT, 2, 0);

  s_ckStatR = lv_label_create(scr);
  lv_obj_set_style_text_font(s_ckStatR, &lv_font_montserrat_16, 0);
  lv_obj_align(s_ckStatR, LV_ALIGN_TOP_RIGHT, -2, 0);

  s_ckBig = lv_label_create(scr);
  lv_obj_set_style_text_font(s_ckBig, &font_clock_100, 0);
  lv_obj_align(s_ckBig, LV_ALIGN_CENTER, 0, 0); // HH:MM centered

  s_ckSec = lv_label_create(scr);
  lv_obj_set_style_text_font(s_ckSec, &lv_font_montserrat_16, 0);
  lv_obj_add_flag(s_ckSec, LV_OBJ_FLAG_HIDDEN); // seconds not shown

  s_ckAmpm = lv_label_create(scr);
  lv_obj_set_style_text_font(s_ckAmpm, &lv_font_montserrat_20, 0);

  s_ckBottom = lv_label_create(scr);
  lv_obj_set_style_text_font(s_ckBottom, &lv_font_montserrat_16, 0);
  lv_obj_align(s_ckBottom, LV_ALIGN_BOTTOM_MID, 0, 0);

  s_cStatL[0] = s_cStatR[0] = '\0';
  s_cBig[0] = s_cSec[0] = s_cAmpm[0] = s_cBottom[0] = '\0';
  refresh_clock(true);
}

// ------------------------------------------------------------- Menu scr ---
enum : uint8_t {
  MENU_ALARM1, MENU_ALARM2, MENU_TZ, MENU_DISPLAY, MENU_TUNES, MENU_SYSINFO,
  MENU_SKY, MENU_PMIC, MENU_TAPSNOOZE, MENU_MODE, MENU_BACK
};
#define MENU_COUNT 11

static void menu_refresh_tapsnooze() {
  if (s_tapLabel)
    lv_label_set_text_fmt(s_tapLabel, "Tap snooze: %s",
                          settings().tapSnooze ? "On" : "Off");
}

static const char *const MODE_NAMES[MODE_COUNT] = {"Alarm clock", "Speedometer",
                                                   "Altimeter", "Game mode"};

static void menu_refresh_mode() {
  if (s_modeLabel)
    lv_label_set_text_fmt(s_modeLabel, "Mode: %s",
                          MODE_NAMES[settings().mode % MODE_COUNT]);
}


static void menu_btn_cb(lv_event_t *e) {
  uint8_t id = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  switch (id) {
  case MENU_ALARM1: s_alarmIdx = 0; load_screen(SCR_ALARM); break;
  case MENU_ALARM2: s_alarmIdx = 1; load_screen(SCR_ALARM); break;
  case MENU_TZ:      load_screen(SCR_TZ); break;
  case MENU_DISPLAY: load_screen(SCR_DISP); break;
  case MENU_TUNES:   load_screen(SCR_TUNES); break;
  case MENU_SYSINFO: load_screen(SCR_SYS); break;
  case MENU_SKY:     load_screen(SCR_SKY); break;
  case MENU_PMIC:    load_screen(SCR_PMIC); break;
  case MENU_TAPSNOOZE: // toggle in place, stay in the menu
    settings().tapSnooze = !settings().tapSnooze;
    settings_save();
    menu_refresh_tapsnooze();
    break;
  case MENU_MODE: // cycle clock/speed/altitude/game in place
    settings().mode = (uint8_t)((settings().mode + 1) % MODE_COUNT);
    settings_save();
    menu_refresh_mode();
    break;
  default:           load_screen(SCR_CLOCK); break;
  }
}

static lv_obj_t *list_add_item(lv_obj_t *list, const char *icon,
                               const char *txt, lv_event_cb_t cb,
                               uint8_t userId) {
  lv_obj_t *btn = lv_list_add_button(list, icon, txt);
  lv_obj_set_style_pad_ver(btn, 6, 0);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(uintptr_t)userId);
  blacken(btn); // list items are added dynamically (tunes) -> blacken on create
  return btn;
}

static void make_menu() {
  lv_obj_t *scr = lv_obj_create(NULL);
  s_scr[SCR_MENU] = scr;
  lv_obj_t *list = lv_list_create(scr);
  lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_all(list, 0, 0);
  lv_obj_set_style_border_width(list, 0, 0);

  static const char *const NAMES[MENU_COUNT] = {
      "Alarm 1", "Alarm 2",    "Time & zone", "Disp & sound", "Tunes",
      "System info", "Sky view", "Battery", "Tap snooze", "Mode",    "Back"};
  for (uint8_t i = 0; i < MENU_COUNT; i++)
    s_fMenu[i] = list_add_item(list, NULL, NAMES[i], menu_btn_cb, i);

  // Inline-state items show their value in the label; grab and set them.
  s_tapLabel = lv_obj_get_child(s_fMenu[MENU_TAPSNOOZE], 0);
  menu_refresh_tapsnooze();
  s_modeLabel = lv_obj_get_child(s_fMenu[MENU_MODE], 0);
  menu_refresh_mode();
}

// -------------------------------------------------------- AlarmEdit scr ---
static void alarm_save_cb(lv_event_t *e) {
  (void)e;
  AlarmConfig &a = settings().alarms[s_alarmIdx];
  a.enabled = lv_obj_has_state(s_alEnable, LV_STATE_CHECKED);
  a.hour = (uint8_t)lv_roller_get_selected(s_alHour);
  a.minute = (uint8_t)lv_roller_get_selected(s_alMin);
  uint8_t mask = 0;
  for (uint8_t i = 0; i < 7; i++)
    if (lv_buttonmatrix_has_button_ctrl(s_alDays, i, LV_BUTTONMATRIX_CTRL_CHECKED))
      mask |= (uint8_t)(1u << i);
  a.daysMask = mask;
  uint16_t sel = lv_dropdown_get_selected(s_alTune);
  if (sel < AUDIO_MELODY_COUNT) {
    a.tune[0] = '\0';
    a.melodyId = (uint8_t)sel;
  } else if ((sel - AUDIO_MELODY_COUNT) < s_tuneCount) {
    strncpy(a.tune, s_tuneNames[sel - AUDIO_MELODY_COUNT], TUNE_NAME_LEN - 1);
    a.tune[TUNE_NAME_LEN - 1] = '\0';
  }
  a.rampSeconds = RAMP_SECONDS[lv_dropdown_get_selected(s_alRamp) & 3];
  a.jitterMinutes = JITTER_MINUTES[lv_dropdown_get_selected(s_alJit) & 3];
  settings_save();
  preview_stop();
  load_screen(SCR_MENU);
}

static void alarm_test_cb(lv_event_t *e) {
  (void)e;
  if (s_previewing) {
    preview_stop();
    return;
  }
  preview_start(lv_dropdown_get_selected(s_alTune), UI_TEST_PREVIEW_MS);
}

static void alarm_sync_widgets() {
  const AlarmConfig &a = settings().alarms[s_alarmIdx];
  lv_label_set_text_fmt(s_alTitle, "Alarm %d", s_alarmIdx + 1);
  if (a.enabled)
    lv_obj_add_state(s_alEnable, LV_STATE_CHECKED);
  else
    lv_obj_remove_state(s_alEnable, LV_STATE_CHECKED);
  lv_roller_set_selected(s_alHour, a.hour <= 23 ? a.hour : 0, LV_ANIM_OFF);
  lv_roller_set_selected(s_alMin, a.minute <= 59 ? a.minute : 0, LV_ANIM_OFF);
  for (uint8_t i = 0; i < 7; i++) {
    if (a.daysMask & (1u << i))
      lv_buttonmatrix_set_button_ctrl(s_alDays, i, LV_BUTTONMATRIX_CTRL_CHECKED);
    else
      lv_buttonmatrix_clear_button_ctrl(s_alDays, i, LV_BUTTONMATRIX_CTRL_CHECKED);
  }
  tunes_scan();
  lv_dropdown_set_options_static(s_alTune, s_tuneOpts);
  uint16_t sel = a.melodyId < AUDIO_MELODY_COUNT ? a.melodyId : 0;
  if (a.tune[0])
    for (uint8_t i = 0; i < s_tuneCount; i++)
      if (nocase_eq(a.tune, s_tuneNames[i])) {
        sel = AUDIO_MELODY_COUNT + i;
        break;
      }
  lv_dropdown_set_selected(s_alTune, sel);
  uint16_t ri = 0;
  for (uint8_t i = 0; i < 4; i++)
    if (a.rampSeconds >= RAMP_SECONDS[i])
      ri = i;
  lv_dropdown_set_selected(s_alRamp, ri);
  uint16_t ji = 0;
  for (uint8_t i = 0; i < 4; i++)
    if (a.jitterMinutes >= JITTER_MINUTES[i])
      ji = i;
  lv_dropdown_set_selected(s_alJit, ji);
}

static void make_alarm() {
  lv_obj_t *scr = make_settings_screen();
  s_scr[SCR_ALARM] = scr;

  s_alTitle = lv_label_create(scr);
  lv_obj_set_style_text_font(s_alTitle, &lv_font_montserrat_20, 0);

  lv_obj_t *row = make_row(scr, "Enabled");
  s_alEnable = lv_switch_create(row);
  lv_obj_set_size(s_alEnable, 48, 24);

  row = make_row(scr, "Hour");
  s_alHour = lv_roller_create(row);
  lv_roller_set_options(s_alHour, s_hourOpts, LV_ROLLER_MODE_NORMAL);
  lv_roller_set_visible_row_count(s_alHour, 1);
  lv_obj_set_width(s_alHour, 64);

  row = make_row(scr, "Minute");
  s_alMin = lv_roller_create(row);
  lv_roller_set_options(s_alMin, s_minOpts, LV_ROLLER_MODE_NORMAL);
  lv_roller_set_visible_row_count(s_alMin, 1);
  lv_obj_set_width(s_alMin, 64);

  s_alDays = lv_buttonmatrix_create(scr);
  lv_buttonmatrix_set_map(s_alDays, DAYS_MAP);
  lv_buttonmatrix_set_button_ctrl_all(s_alDays, LV_BUTTONMATRIX_CTRL_CHECKABLE);
  lv_obj_set_size(s_alDays, LV_PCT(100), 40);
  lv_obj_set_style_pad_all(s_alDays, 1, 0);
  lv_obj_set_style_pad_gap(s_alDays, 2, 0);
  lv_obj_set_style_text_font(s_alDays, &lv_font_montserrat_16, LV_PART_ITEMS);

  row = make_row(scr, "Tune");
  s_alTune = lv_dropdown_create(row);
  lv_obj_set_width(s_alTune, 220);
  lv_obj_set_style_text_font(lv_dropdown_get_list(s_alTune),
                             &lv_font_montserrat_16, 0);
  blacken(lv_dropdown_get_list(s_alTune)); // popup list is off-screen tree

  row = make_row(scr, "Ramp");
  s_alRamp = lv_dropdown_create(row);
  lv_dropdown_set_options_static(s_alRamp, RAMP_OPTS);
  lv_obj_set_width(s_alRamp, 140);
  lv_obj_set_style_text_font(lv_dropdown_get_list(s_alRamp),
                             &lv_font_montserrat_16, 0);
  blacken(lv_dropdown_get_list(s_alRamp));

  row = make_row(scr, "Random");
  s_alJit = lv_dropdown_create(row);
  lv_dropdown_set_options_static(s_alJit, JITTER_OPTS);
  lv_obj_set_width(s_alJit, 150);
  lv_obj_set_style_text_font(lv_dropdown_get_list(s_alJit),
                             &lv_font_montserrat_16, 0);
  blacken(lv_dropdown_get_list(s_alJit));

  row = make_row(scr, NULL);
  s_alTest = lv_button_create(row);
  lv_obj_t *l = lv_label_create(s_alTest);
  lv_label_set_text_static(l, "Test");
  s_alSave = lv_button_create(row);
  l = lv_label_create(s_alSave);
  lv_label_set_text_static(l, "Save");
  lv_obj_add_event_cb(s_alTest, alarm_test_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(s_alSave, alarm_save_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *foc[9] = {s_alEnable, s_alHour, s_alMin, s_alDays, s_alTune,
                      s_alRamp,   s_alJit,  s_alTest, s_alSave};
  for (uint8_t i = 0; i < 9; i++) {
    s_fAlarm[i] = foc[i];
    lv_obj_add_flag(foc[i], LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  }
}

// ------------------------------------------------------ Time & zone scr ---
static void tz_auto_cb(lv_event_t *e) {
  (void)e;
  bool on = lv_obj_has_state(s_tzAuto, LV_STATE_CHECKED);
  settings().tzAuto = on;
  if (on)
    lv_obj_add_state(s_tzZone, LV_STATE_DISABLED);
  else
    lv_obj_remove_state(s_tzZone, LV_STATE_DISABLED);
  mark_dirty();
}

static void tz_zone_cb(lv_event_t *e) {
  (void)e;
  uint16_t sel = lv_dropdown_get_selected(s_tzZone);
  if (sel >= TZ_COUNT)
    return;
  strncpy(settings().tzPosix, TZ_TABLE[sel].posix, TZ_POSIX_LEN - 1);
  settings().tzPosix[TZ_POSIX_LEN - 1] = '\0';
  strncpy(settings().tzName, TZ_TABLE[sel].name, TZ_NAME_LEN - 1);
  settings().tzName[TZ_NAME_LEN - 1] = '\0';
  mark_dirty();
}

static void tz_24h_cb(lv_event_t *e) {
  (void)e;
  settings().use24h = lv_obj_has_state(s_tz24, LV_STATE_CHECKED);
  mark_dirty();
}

static void tz_sync_cb(lv_event_t *e) {
  (void)e;
  clock_request_sync();
  s_tzNoteUntil = millis() + 2000;
  s_cTzInfo[0] = '\0'; // force info refresh
  lv_label_set_text_static(s_tzInfo, "sync requested");
}

static void refresh_tz_info(bool force) {
  if (s_tzNoteUntil) {
    if ((int32_t)(millis() - s_tzNoteUntil) < 0)
      return;
    s_tzNoteUntil = 0;
  }
  int32_t off = clock_tz_offset();
  char sign = off < 0 ? '-' : '+';
  uint32_t ao = off < 0 ? (uint32_t)-off : (uint32_t)off;
  char b[64];
  snprintf(b, sizeof(b), "%s  UTC%c%02lu:%02lu%s", settings().tzName, sign,
           (unsigned long)(ao / 3600), (unsigned long)((ao % 3600) / 60),
           clock_is_dst() ? " DST" : "");
  set_label_if(s_tzInfo, s_cTzInfo, sizeof(s_cTzInfo), b, force);
}

static void tz_sync_widgets() {
  if (settings().tzAuto) {
    lv_obj_add_state(s_tzAuto, LV_STATE_CHECKED);
    lv_obj_add_state(s_tzZone, LV_STATE_DISABLED);
  } else {
    lv_obj_remove_state(s_tzAuto, LV_STATE_CHECKED);
    lv_obj_remove_state(s_tzZone, LV_STATE_DISABLED);
  }
  // Show plain "GMT" when the saved zone (usually an auto/GNSS-derived named
  // zone with DST) has no exact match in the fixed-offset ladder.
  uint16_t sel = 0;
  for (uint16_t i = 0; i < TZ_COUNT; i++)
    if (strcmp(TZ_TABLE[i].name, "GMT") == 0) {
      sel = i;
      break;
    }
  for (uint16_t i = 0; i < TZ_COUNT; i++)
    if (strcmp(settings().tzPosix, TZ_TABLE[i].posix) == 0) {
      sel = i;
      break;
    }
  lv_dropdown_set_selected(s_tzZone, sel);
  if (settings().use24h)
    lv_obj_add_state(s_tz24, LV_STATE_CHECKED);
  else
    lv_obj_remove_state(s_tz24, LV_STATE_CHECKED);
  s_tzNoteUntil = 0;
  refresh_tz_info(true);
}

static void make_timezone() {
  lv_obj_t *scr = make_settings_screen();
  s_scr[SCR_TZ] = scr;

  lv_obj_t *row = make_row(scr, "Auto TZ (GNSS)");
  s_tzAuto = lv_switch_create(row);
  lv_obj_set_size(s_tzAuto, 48, 24);
  lv_obj_add_event_cb(s_tzAuto, tz_auto_cb, LV_EVENT_VALUE_CHANGED, NULL);

  row = make_row(scr, "Zone");
  s_tzZone = lv_dropdown_create(row);
  lv_dropdown_set_options_static(s_tzZone, TZ_OPTS);
  lv_obj_set_width(s_tzZone, 200);
  lv_obj_set_style_text_font(lv_dropdown_get_list(s_tzZone),
                             &lv_font_montserrat_16, 0);
  blacken(lv_dropdown_get_list(s_tzZone));
  lv_obj_add_event_cb(s_tzZone, tz_zone_cb, LV_EVENT_VALUE_CHANGED, NULL);

  row = make_row(scr, "24h clock");
  s_tz24 = lv_switch_create(row);
  lv_obj_set_size(s_tz24, 48, 24);
  lv_obj_add_event_cb(s_tz24, tz_24h_cb, LV_EVENT_VALUE_CHANGED, NULL);

  row = make_row(scr, NULL);
  s_tzSync = lv_button_create(row);
  lv_obj_t *l = lv_label_create(s_tzSync);
  lv_label_set_text_static(l, "Sync now");
  lv_obj_add_event_cb(s_tzSync, tz_sync_cb, LV_EVENT_CLICKED, NULL);
  s_tzInfo = lv_label_create(row);
  lv_obj_set_style_text_font(s_tzInfo, &lv_font_montserrat_16, 0);
  s_cTzInfo[0] = '\0';

  lv_obj_t *foc[4] = {s_tzAuto, s_tzZone, s_tz24, s_tzSync};
  for (uint8_t i = 0; i < 4; i++) {
    s_fTz[i] = foc[i];
    lv_obj_add_flag(foc[i], LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  }
}

// ---------------------------------------------------------- Display scr ---
static void disp_vol_cb(lv_event_t *e) {
  (void)e;
  uint8_t v = (uint8_t)lv_slider_get_value(s_dVolume);
  settings().volume = v;
  if (amp_enabled())
    amp_set_volume(v); // live while previewing/ringing
  mark_dirty();
}

static void disp_bright_cb(lv_event_t *e) {
  (void)e;
  uint8_t v = (uint8_t)lv_slider_get_value(s_dBright);
  settings().brightness = v;
  display_set_contrast(v); // live
  mark_dirty();
}

static void disp_dim_cb(lv_event_t *e) {
  (void)e;
  uint16_t sel = lv_dropdown_get_selected(s_dDim);
  if (sel < 5) {
    settings().dimTimeoutS = DIM_SECONDS[sel];
    mark_dirty();
  }
}

static void disp_dimlvl_cb(lv_event_t *e) {
  (void)e;
  settings().dimBrightness = (uint8_t)lv_slider_get_value(s_dDimLvl);
  mark_dirty();
}

static void disp_starry_cb(lv_event_t *e) {
  (void)e;
  settings().starryNight = lv_obj_has_state(s_dStarry, LV_STATE_CHECKED);
  mark_dirty();
}

static void disp_sync_widgets() {
  if (settings().starryNight)
    lv_obj_add_state(s_dStarry, LV_STATE_CHECKED);
  else
    lv_obj_remove_state(s_dStarry, LV_STATE_CHECKED);
  lv_slider_set_value(s_dVolume, settings().volume, LV_ANIM_OFF);
  lv_slider_set_value(s_dBright, settings().brightness, LV_ANIM_OFF);
  uint16_t t = settings().dimTimeoutS;
  uint16_t sel = 0;
  for (uint16_t i = 1; i < 5; i++)
    if (t >= DIM_SECONDS[i])
      sel = i;
  if (t == 0)
    sel = 0;
  lv_dropdown_set_selected(s_dDim, sel);
  lv_slider_set_value(s_dDimLvl, settings().dimBrightness, LV_ANIM_OFF);
}

static void make_display() {
  lv_obj_t *scr = make_settings_screen();
  s_scr[SCR_DISP] = scr;

  lv_obj_t *row = make_row(scr, "Volume");
  s_dVolume = lv_slider_create(row);
  lv_slider_set_range(s_dVolume, 0, 10);
  lv_obj_set_size(s_dVolume, 220, 12);
  lv_obj_add_event_cb(s_dVolume, disp_vol_cb, LV_EVENT_VALUE_CHANGED, NULL);

  row = make_row(scr, "Brightness");
  s_dBright = lv_slider_create(row);
  lv_slider_set_range(s_dBright, 5, 255);
  lv_obj_set_size(s_dBright, 220, 12);
  lv_obj_add_event_cb(s_dBright, disp_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);

  row = make_row(scr, "Dim after");
  s_dDim = lv_dropdown_create(row);
  lv_dropdown_set_options_static(s_dDim, DIM_OPTS);
  lv_obj_set_width(s_dDim, 140);
  lv_obj_set_style_text_font(lv_dropdown_get_list(s_dDim),
                             &lv_font_montserrat_16, 0);
  blacken(lv_dropdown_get_list(s_dDim));
  lv_obj_add_event_cb(s_dDim, disp_dim_cb, LV_EVENT_VALUE_CHANGED, NULL);

  row = make_row(scr, "Dim level");
  s_dDimLvl = lv_slider_create(row);
  lv_slider_set_range(s_dDimLvl, 0, 255);
  lv_obj_set_size(s_dDimLvl, 220, 12);
  lv_obj_add_event_cb(s_dDimLvl, disp_dimlvl_cb, LV_EVENT_VALUE_CHANGED, NULL);

  row = make_row(scr, "Starry night");
  s_dStarry = lv_switch_create(row);
  lv_obj_set_size(s_dStarry, 48, 24);
  lv_obj_add_event_cb(s_dStarry, disp_starry_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *foc[5] = {s_dVolume, s_dBright, s_dDim, s_dDimLvl, s_dStarry};
  for (uint8_t i = 0; i < 5; i++) {
    s_fDisp[i] = foc[i];
    lv_obj_add_flag(foc[i], LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  }
}

// ------------------------------------------------------------ Tunes scr ---
static void tune_item_cb(lv_event_t *e) {
  uint16_t idx = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
  if (s_previewing) {
    preview_stop();
    return;
  }
  preview_start(idx, 0);
}

static void tunes_rebuild() {
  lv_obj_clean(s_tuneList); // deleted buttons auto-leave the group
  tunes_scan();
  s_nfTunes = 0;
  for (uint8_t i = 0; i < AUDIO_MELODY_COUNT; i++)
    s_fTunes[s_nfTunes++] = list_add_item(s_tuneList, LV_SYMBOL_AUDIO,
                                          AUDIO_MELODY_NAMES[i], tune_item_cb,
                                          i);
  for (uint8_t i = 0; i < s_tuneCount; i++)
    s_fTunes[s_nfTunes++] =
        list_add_item(s_tuneList, LV_SYMBOL_FILE, s_tuneNames[i], tune_item_cb,
                      (uint8_t)(AUDIO_MELODY_COUNT + i));
}

static void make_tunes() {
  lv_obj_t *scr = lv_obj_create(NULL);
  s_scr[SCR_TUNES] = scr;
  s_tuneList = lv_list_create(scr);
  lv_obj_set_size(s_tuneList, LV_PCT(100), LV_PCT(100));
  lv_obj_align(s_tuneList, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_all(s_tuneList, 0, 0);
  lv_obj_set_style_border_width(s_tuneList, 0, 0);
}

// ---------------------------------------------------------- SysInfo scr ---
static void refresh_sysinfo(bool force) {
  char lat[16], lon[16], hdop[8], age[16], spd[24], alt[24], b[400];
  float la, lo;
  bool havePos = gnss_get_position(la, lo);
  if (havePos) {
    fmt_coord(lat, sizeof(lat), la);
    fmt_coord(lon, sizeof(lon), lo);
  } else {
    strcpy(lat, "--");
    strcpy(lon, "--");
  }
  uint16_t hx = gnss_hdop_x10();
  if (hx == 0xFFFF)
    strcpy(hdop, "--");
  else
    snprintf(hdop, sizeof(hdop), "%u.%u", hx / 10, hx % 10);
  // Speed & altitude (integer-formatted; this build has no %f in printf).
  float spKmph, altM;
  if (gnss_get_speed_kmph(spKmph)) {
    unsigned s10 = (unsigned)(spKmph * 10.0f + 0.5f);
    snprintf(spd, sizeof(spd), "%u.%u km/h", s10 / 10u, s10 % 10u);
  } else {
    strcpy(spd, "--");
  }
  if (gnss_get_altitude_m(altM)) {
    int m = (int)(altM >= 0.0f ? altM + 0.5f : altM - 0.5f);
    snprintf(alt, sizeof(alt), "%d m", m);
  } else {
    strcpy(alt, "--");
  }
  const char *ampStatus = "off (idle)";
  {
    bool fault = false, thermal = false;
    if (amp_get_status(fault, thermal))
      ampStatus = thermal ? "THERMAL" : fault ? "FAULT (output)" : "ok";
    else if (!amp_present())
      ampStatus = "MISSING";
  }
  uint32_t a = clock_last_gnss_sync_age_s();
  if (a == UINT32_MAX)
    strcpy(age, "never");
  else if (a < 120)
    snprintf(age, sizeof(age), "%lus", (unsigned long)a);
  else if (a < 7200)
    snprintf(age, sizeof(age), "%lum", (unsigned long)(a / 60));
  else
    snprintf(age, sizeof(age), "%luh", (unsigned long)(a / 3600));

  int32_t off = clock_tz_offset();
  char sign = off < 0 ? '-' : '+';
  uint32_t ao = off < 0 ? (uint32_t)-off : (uint32_t)off;

  snprintf(b, sizeof(b),
           "Fix %s   Sats %u   HDOP %s\n"
           "NMEA %lu chars\n"
           "Lat %s   Lon %s\n"
           "Speed %s   Alt %s\n"
           "TZ %s\n"
           "%s\n"
           "Offset %c%02lu:%02lu%s\n"
           "GNSS sync %s   RTC %s\n"
           "Caps %s\n"
           "Snoozed %u this week, %lu total\n"
           "Amp %s\n"
           "FW " UI_FW_VERSION " " __DATE__,
           gnss_has_fix() ? "yes" : "no", (unsigned)gnss_num_sats(), hdop,
           (unsigned long)gnss_chars_seen(), lat,
           lon, spd, alt, settings().tzName, settings().tzPosix, sign,
           (unsigned long)(ao / 3600), (unsigned long)((ao % 3600) / 60),
           clock_is_dst() ? " DST" : "", age, rtc_present() ? "ok" : "MISSING",
           supercaps_ready() ? "ready" : "charging",
           (unsigned)alarm_snoozes_this_week(),
           (unsigned long)settings().snoozeTotal, ampStatus);
  if (force || strcmp(s_cSys, b) != 0) {
    snprintf(s_cSys, sizeof(s_cSys), "%s", b);
    lv_label_set_text(s_sysLabel, b);
  }
}

static void make_sysinfo() {
  lv_obj_t *scr = lv_obj_create(NULL);
  s_scr[SCR_SYS] = scr;
  lv_obj_set_style_pad_all(scr, 2, 0);
  s_sysLabel = lv_label_create(scr);
  lv_obj_set_width(s_sysLabel, LV_PCT(100));
  lv_obj_set_style_text_font(s_sysLabel, &lv_font_montserrat_14, 0);
  lv_label_set_long_mode(s_sysLabel, LV_LABEL_LONG_MODE_WRAP);
  s_cSys[0] = '\0';
}

// ------------------------------------------------------ Battery/PMIC scr ---
// v2 boards: live nPM1300 readout — VBUS, battery voltage + rough estimate,
// charge state, die temperature. v1 boards show a "no PMIC" note.
static void refresh_pmic(bool force) {
  char b[220];
  PmicStatus st;
  if (!pmic_present()) {
    snprintf(b, sizeof(b), "No PMIC on this board (v1).\n"
                           "Power: LTC3226 supercaps, %s",
             supercaps_ready() ? "ready" : "charging");
  } else if (!pmic_read_status(st)) {
    snprintf(b, sizeof(b), "nPM1300: read error");
  } else {
    // Rough SoC from voltage, scaled to the configured termination voltage
    // (linear 3.5 V..vterm). Reads high while charging.
    int span = (int)pmic_vterm_mv() - 3500;
    int pct = ((int)st.vbatMv - 3500) * 100 / (span > 0 ? span : 700);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    snprintf(b, sizeof(b),
             "USB power: %s (limit 500 mA%s)\n"
             "Battery: %u.%02u V  (~%d%% est)\n"
             "Charger: %s @ %u mA -> %u.%02u V\n"
             "PMIC die: %d.%d C\n"
             "Status raw: 0x%02X",
             st.vbusPresent ? "present" : "absent",
             pmic_vbus_500() ? "" : ", RAISE FAILED",
             st.vbatMv / 1000, (st.vbatMv % 1000) / 10, pct,
             pmic_charge_text(st.chargeStatus), pmic_charge_current_ma(),
             pmic_vterm_mv() / 1000, (pmic_vterm_mv() % 1000) / 10,
             st.dieTempCx10 / 10, abs(st.dieTempCx10 % 10), st.chargeStatus);
  }
  if (force || strcmp(s_cPmic, b) != 0) {
    snprintf(s_cPmic, sizeof(s_cPmic), "%s", b);
    lv_label_set_text(s_pmicLabel, b);
  }
}

static void make_pmic() {
  lv_obj_t *scr = lv_obj_create(NULL);
  s_scr[SCR_PMIC] = scr;
  lv_obj_set_style_pad_all(scr, 4, 0);
  s_pmicLabel = lv_label_create(scr);
  lv_obj_set_width(s_pmicLabel, LV_PCT(100));
  lv_obj_set_style_text_font(s_pmicLabel, &lv_font_montserrat_16, 0);
  lv_label_set_long_mode(s_pmicLabel, LV_LABEL_LONG_MODE_WRAP);
  s_cPmic[0] = '\0';
}

// --------------------------------------------------------- Sky view scr ---
// Left: polar plot of the visible sky — center = zenith, outer ring =
// horizon, north up. Right: SNR bars for the SKY_MAX_BARS strongest sats.
// Dot/bar color = signal quality; label/border color = constellation
// (white GPS, cyan GLONASS). Data refreshes with each GSV group (~5 s).
#define SKY_CX 71
#define SKY_CY 71
#define SKY_R 63
#define SKY_BARS_X 158
#define SKY_BAR_W 14
#define SKY_BAR_PITCH 22
#define SKY_BAR_BOTTOM 118

static lv_color_t sky_snr_color(uint8_t snr) {
  if (snr == 0) // in view (almanac) but not tracked
    return lv_palette_darken(LV_PALETTE_GREY, 1);
  if (snr < 20)
    return lv_palette_main(LV_PALETTE_RED);
  if (snr < 35)
    return lv_palette_main(LV_PALETTE_YELLOW);
  return lv_palette_main(LV_PALETTE_GREEN);
}

static lv_color_t sky_sys_color(char system) {
  return system == 'R' ? lv_palette_main(LV_PALETTE_CYAN) : lv_color_white();
}

static void refresh_sky(bool force) {
  GnssSatInfo sats[SKY_MAX_SATS];
  uint8_t n = gnss_get_sats(sats, SKY_MAX_SATS);
  if (!force && n == s_skyLastN &&
      memcmp(sats, s_skyLast, n * sizeof(GnssSatInfo)) == 0)
    return;
  memcpy(s_skyLast, sats, n * sizeof(GnssSatInfo));
  s_skyLastN = n;

  uint8_t tracked = 0;
  for (uint8_t i = 0; i < n; i++)
    if (sats[i].snrDb > 0)
      tracked++;
  if (n == 0)
    lv_label_set_text_static(s_skyInfo, "waiting for GSV data...");
  else
    lv_label_set_text_fmt(s_skyInfo, "%u in view, %u tracked", (unsigned)n,
                          (unsigned)tracked);

  for (uint8_t i = 0; i < SKY_MAX_SATS; i++) {
    if (i < n) {
      // elevation -> radius (zenith at center), azimuth -> angle from north
      float r = (float)SKY_R * (90.0f - (float)sats[i].elevDeg) / 90.0f;
      float a = (float)sats[i].azimDeg * 0.017453293f;
      int32_t x = SKY_CX + (int32_t)lroundf(r * sinf(a));
      int32_t y = SKY_CY - (int32_t)lroundf(r * cosf(a));
      lv_obj_set_pos(s_skyDots[i], x - 4, y - 4);
      lv_obj_set_style_bg_color(s_skyDots[i], sky_snr_color(sats[i].snrDb), 0);
      lv_obj_set_style_border_color(s_skyDots[i], sky_sys_color(sats[i].system),
                                    0);
      lv_obj_remove_flag(s_skyDots[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_skyDots[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  // Bars show the strongest sats first (insertion sort by SNR, descending).
  uint8_t order[SKY_MAX_SATS];
  for (uint8_t i = 0; i < n; i++) {
    uint8_t j = i;
    while (j && sats[order[j - 1]].snrDb < sats[i].snrDb) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = i;
  }
  for (uint8_t i = 0; i < SKY_MAX_BARS; i++) {
    if (i < n) {
      const GnssSatInfo &s = sats[order[i]];
      int32_t h = 6 + (int32_t)s.snrDb * 82 / 55; // 0..55 dB-Hz -> 6..88 px
      if (h > 88)
        h = 88;
      lv_obj_set_size(s_skyBars[i], SKY_BAR_W, h);
      lv_obj_set_pos(s_skyBars[i], SKY_BARS_X + i * SKY_BAR_PITCH,
                     SKY_BAR_BOTTOM - h);
      lv_obj_set_style_bg_color(s_skyBars[i], sky_snr_color(s.snrDb), 0);
      lv_label_set_text_fmt(s_skyBarLbl[i], "%u", (unsigned)s.prn);
      lv_obj_set_style_text_color(s_skyBarLbl[i], sky_sys_color(s.system), 0);
      lv_obj_remove_flag(s_skyBars[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(s_skyBarLbl[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_skyBars[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_skyBarLbl[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void make_sky() {
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  s_scr[SCR_SKY] = scr;

  // Elevation rings: horizon (0 deg) and 45 deg
  static const int32_t RADII[2] = {SKY_R, SKY_R / 2};
  for (int i = 0; i < 2; i++) {
    lv_obj_t *ring = lv_obj_create(scr);
    lv_obj_set_size(ring, RADII[i] * 2, RADII[i] * 2);
    lv_obj_set_pos(ring, SKY_CX - RADII[i], SKY_CY - RADII[i]);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, lv_palette_darken(LV_PALETTE_GREY, 2),
                                  0);
    lv_obj_remove_flag(ring, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  }
  lv_obj_t *north = lv_label_create(scr);
  lv_obj_set_style_text_font(north, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(north, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_label_set_text_static(north, "N");
  lv_obj_set_pos(north, SKY_CX - 5, SKY_CY - SKY_R + 1);

  for (uint8_t i = 0; i < SKY_MAX_SATS; i++) {
    lv_obj_t *d = lv_obj_create(scr);
    lv_obj_set_size(d, 9, 9);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d, 1, 0);
    lv_obj_set_style_pad_all(d, 0, 0);
    lv_obj_remove_flag(d, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    s_skyDots[i] = d;
  }

  s_skyInfo = lv_label_create(scr);
  lv_obj_set_style_text_font(s_skyInfo, &lv_font_montserrat_16, 0);
  lv_obj_set_pos(s_skyInfo, SKY_BARS_X, 2);

  for (uint8_t i = 0; i < SKY_MAX_BARS; i++) {
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_remove_flag(bar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    s_skyBars[i] = bar;

    lv_obj_t *lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, SKY_BAR_PITCH);
    lv_obj_set_pos(lbl, SKY_BARS_X + i * SKY_BAR_PITCH - 4, SKY_BAR_BOTTOM + 4);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    s_skyBarLbl[i] = lbl;
  }
  s_skyLastN = 255; // force first refresh to lay everything out
}

// ---------------------------------------------------------- Ringing scr ---
static void make_ringing() {
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  s_scr[SCR_RING] = scr;

  s_rgTitle = lv_label_create(scr);
  lv_obj_set_style_text_font(s_rgTitle, &lv_font_montserrat_16, 0);
  lv_obj_align(s_rgTitle, LV_ALIGN_TOP_MID, 0, 0);
  lv_label_set_text_static(s_rgTitle, "ALARM");

  s_rgTime = lv_label_create(scr);
  lv_obj_set_style_text_font(s_rgTime, &font_clock_100, 0);
  lv_obj_align(s_rgTime, LV_ALIGN_CENTER, 0, 2);

  s_rgHint = lv_label_create(scr);
  lv_obj_set_style_text_font(s_rgHint, &lv_font_montserrat_16, 0);
  lv_obj_align(s_rgHint, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_label_set_text_static(s_rgHint, "press = snooze    hold = stop");
}

// ------------------------------------------------------- screen loading ---
static void load_screen(UiScreen id) {
  close_msgbox();
  force_exit_edit();
  if (id != SCR_TUNES && id != SCR_ALARM)
    preview_stop();

  switch (id) {
  case SCR_CLOCK: refresh_clock(true); group_set(NULL, 0); break;
  case SCR_MENU:
    menu_refresh_tapsnooze();
    menu_refresh_mode();
    group_set(s_fMenu, MENU_COUNT);
    break;
  case SCR_ALARM: alarm_sync_widgets(); group_set(s_fAlarm, 9); break;
  case SCR_TZ:    tz_sync_widgets(); group_set(s_fTz, 4); break;
  case SCR_DISP:  disp_sync_widgets(); group_set(s_fDisp, 5); break;
  case SCR_TUNES:
    tunes_rebuild();
    group_set(s_fTunes, s_nfTunes);
    if (storage_busy())
      show_msgbox("USB busy", "WAV files are hidden while the\nUSB host owns the drive.");
    break;
  case SCR_SYS:
    refresh_sysinfo(true);
    lv_obj_scroll_to_y(s_scr[SCR_SYS], 0, LV_ANIM_OFF);
    group_set(NULL, 0);
    break;
  case SCR_SKY:
    refresh_sky(true);
    group_set(NULL, 0);
    break;
  case SCR_PMIC:
    refresh_pmic(true);
    group_set(NULL, 0);
    break;
  case SCR_RING:  group_set(NULL, 0); break;
  }
  s_screen = id;
  lv_screen_load(s_scr[id]);
}

// ------------------------------------------------------------ edit mode ---

// A dropdown list has nowhere sensible to open on a 76 px panel: LVGL sees
// too little room below the row, flips the list above it and clips it at the
// screen edge. Instead, re-anchor the just-opened list as a TOP overlay:
// y = 0, up to the full screen height, with the selected option scrolled to
// the middle (same math as LVGL's own position_to_selected, which ran before
// the resize). x/width are left where LVGL aligned them (under the widget).
static void dropdown_overlay(lv_obj_t *dd) {
  lv_obj_t *list = lv_dropdown_get_list(dd);
  if (!list)
    return;
  lv_obj_set_height(list, LV_SIZE_CONTENT);
  lv_obj_update_layout(list);
  int32_t h = lv_obj_get_height(list);
  if (h > DISP_H)
    h = DISP_H;
  lv_obj_set_height(list, h);
  // y in CONTENT coordinates: the settings screens scroll, so "top of the
  // visible viewport" = the parent's current scroll offset, not 0. (Found
  // the hard way: lists opened from scrolled-down rows appeared off-glass.)
  lv_obj_t *par = lv_obj_get_parent(list);
  lv_obj_set_y(list, par ? lv_obj_get_scroll_y(par) : 0);
  lv_obj_update_layout(list);
  lv_obj_t *label = lv_obj_get_child(list, 0); // options live in one label
  const lv_font_t *font = lv_obj_get_style_text_font(list, LV_PART_MAIN);
  int32_t lineH = lv_font_get_line_height(font) +
                  (label ? lv_obj_get_style_text_line_space(label, LV_PART_MAIN)
                         : 0);
  int32_t sel = (int32_t)lv_dropdown_get_selected(dd);
  lv_obj_scroll_to_y(list, sel * lineH - (h / 2 - lineH / 2), LV_ANIM_OFF);
}

static void enter_edit(lv_obj_t *f) {
  s_editing = true;
  lv_obj_add_state(f, LV_STATE_EDITED);
  if (lv_obj_check_type(f, &lv_dropdown_class)) {
    // Open directly (a queued LV_KEY_ENTER would only open on the next indev
    // read — too late to reposition the list in the same frame).
    lv_dropdown_open(f);
    dropdown_overlay(f);
  } else if (lv_obj_check_type(f, &lv_roller_class)) {
    s_rollerOrig = (uint16_t)lv_roller_get_selected(f); // for commit/cancel
  }
}

static void exit_edit() {
  lv_obj_t *f = lv_group_get_focused(s_group);
  if (f)
    lv_obj_remove_state(f, LV_STATE_EDITED);
  s_editing = false;
}

static void handle_edit_key(const ButtonEvent &ev) {
  lv_obj_t *f = lv_group_get_focused(s_group);
  if (!f || !is_editable(f)) {
    s_editing = false;
    return;
  }
  bool slider = lv_obj_check_type(f, &lv_slider_class);
  bool dd = lv_obj_check_type(f, &lv_dropdown_class);
  bool bm = lv_obj_check_type(f, &lv_buttonmatrix_class);
  bool roller = lv_obj_check_type(f, &lv_roller_class);

  // Long-press B2/B3 = coarse step (big lists like the 67-entry zone
  // dropdown, 0..255 sliders). Buttons emit Long instead of Short, so the two
  // rates don't stack. Dropdowns are stepped by queued keys; 5 fits the
  // 8-deep key queue. Buttonmatrix (weekdays) keeps single steps.
  bool big = ev.action == BtnAction::Long;
  // Small-range sliders (volume 0..10) step 1/3; wide ones (0..255) 10/50.
  int sfine = 10, scoarse = 50;
  if (slider) {
    int span = lv_slider_get_max_value(f) - lv_slider_get_min_value(f);
    if (span <= 20) { sfine = 1; scoarse = 3; }
  }
  switch (ev.id) {
  case BtnId::B2: // decrease / previous
    if (slider)
      slider_step(f, big ? -scoarse : -sfine);
    else if (roller)
      roller_step(f, big ? -10 : -1);
    else
      for (int i = 0; i < ((dd && big) ? 5 : 1); i++)
        push_key(LV_KEY_LEFT); // dropdown list / btnmatrix: move left
    break;
  case BtnId::B3: // increase / next
    if (slider)
      slider_step(f, big ? +scoarse : +sfine);
    else if (roller)
      roller_step(f, big ? +10 : +1);
    else
      for (int i = 0; i < ((dd && big) ? 5 : 1); i++)
        push_key(LV_KEY_RIGHT);
    break;
  case BtnId::B4:
    if (bm) {
      push_key(LV_KEY_ENTER); // toggle the selected day, stay in edit mode
    } else {
      if (dd)
        push_key(LV_KEY_ENTER); // commit selection, closes the list
      else if (roller)
        // Commit: the roller reverts to its shadowed value on defocus; keys
        // don't update that shadow, so re-set the selection to lock it in.
        lv_roller_set_selected(f, lv_roller_get_selected(f), LV_ANIM_OFF);
      exit_edit();
    }
    break;
  case BtnId::B1:
    if (dd)
      push_key(LV_KEY_ESC); // revert + close list
    else if (roller)
      lv_roller_set_selected(f, s_rollerOrig, LV_ANIM_OFF); // cancel -> revert
    exit_edit();
    break;
  }
}

static void nav_back() {
  switch (s_screen) {
  case SCR_MENU:
    load_screen(SCR_CLOCK);
    break;
  case SCR_ALARM: // back without saving
  case SCR_TZ:
  case SCR_DISP:
  case SCR_TUNES:
  case SCR_SYS:
  case SCR_SKY:
  case SCR_PMIC:
    load_screen(SCR_MENU);
    break;
  default:
    break;
  }
}

// ----------------------------------------------------------- public API ---
// True-black background for the OLED: after the default dark theme has styled
// every object, recursively force each object's MAIN part (default state
// only) to a fully-off black fill. Focus/checked state styles carry higher
// specificity, so the theme's selection highlight still shows through —
// navigation stays visible, and slider/switch knobs+indicators (other parts)
// are untouched. (v9's lv_theme_t is opaque, so we style the tree directly
// instead of wrapping the theme's apply_cb.)
static lv_style_t s_stBlack;

static void blacken(lv_obj_t *o) {
  lv_obj_add_style(o, &s_stBlack, LV_PART_MAIN);
  uint32_t n = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < n; i++)
    blacken(lv_obj_get_child(o, i));
}

void ui_begin() {
  // Prepare the true-black MAIN-part style once; applied to every screen
  // after they are built (see the blacken() loop below).
  lv_style_init(&s_stBlack);
  lv_style_set_bg_color(&s_stBlack, lv_color_black());
  lv_style_set_bg_opa(&s_stBlack, LV_OPA_COVER);

  s_group = lv_group_create();
  lv_group_set_default(s_group);

  s_indev = lv_indev_create();
  lv_indev_set_type(s_indev, LV_INDEV_TYPE_KEYPAD);
  lv_indev_set_read_cb(s_indev, keypad_read_cb);
  lv_indev_set_group(s_indev, s_group);

  // "00\n01\n...\n23" / "...59" (last entry without trailing \n)
  char *p = s_hourOpts;
  for (uint8_t i = 0; i < 24; i++)
    p += sprintf(p, i < 23 ? "%02u\n" : "%02u", i);
  p = s_minOpts;
  for (uint8_t i = 0; i < 60; i++)
    p += sprintf(p, i < 59 ? "%02u\n" : "%02u", i);

  make_clock();
  make_menu();
  make_alarm();
  make_timezone();
  make_display();
  make_tunes();
  make_sysinfo();
  make_sky();
  make_pmic();
  make_ringing();

  // Force every screen's MAIN part to true black now that all are built.
  for (int i = 0; i < SCR_COUNT; i++)
    blacken(s_scr[i]);

  s_lastActMs = millis();
  load_screen(SCR_CLOCK);
}

void ui_handle_event(const ButtonEvent &ev) {
  if (s_screen == SCR_RING)
    return; // main.cpp intercepts while ringing; ignore stragglers

  if (s_msgbox) { // modal: any press dismisses
    close_msgbox();
    return;
  }

  if (ev.id == BtnId::B1 && ev.action == BtnAction::Long) { // go home
    preview_stop();
    load_screen(SCR_CLOCK);
    return;
  }

  if (s_screen == SCR_CLOCK) {
    // Game mode: B2..B4 belong to the USB gamepad (read raw by UsbGamepad),
    // so only B1 opens the menu. Other modes: B1 or B4 opens it.
    if (ev.id == BtnId::B1 ||
        (ev.id == BtnId::B4 && settings().mode != MODE_GAME))
      load_screen(SCR_MENU);
    return;
  }

  // A running preview: ESC or ENTER stops it (Tunes toggle / Test abort).
  if (s_previewing && (ev.id == BtnId::B1 || ev.id == BtnId::B4)) {
    preview_stop();
    return;
  }

  if (s_editing) {
    handle_edit_key(ev);
    return;
  }

  switch (ev.id) {
  case BtnId::B2:
    if (s_screen == SCR_SYS)
      lv_obj_scroll_by(s_scr[SCR_SYS], 0, 16, LV_ANIM_OFF);
    else
      push_key(LV_KEY_PREV);
    break;
  case BtnId::B3:
    if (s_screen == SCR_SYS)
      lv_obj_scroll_by(s_scr[SCR_SYS], 0, -16, LV_ANIM_OFF);
    else
      push_key(LV_KEY_NEXT);
    break;
  case BtnId::B4: { // short or long both act as ENTER
    lv_obj_t *f = lv_group_get_focused(s_group);
    if (is_editable(f))
      enter_edit(f);
    else
      push_key(LV_KEY_ENTER);
    break;
  }
  case BtnId::B1: // short press = back
    nav_back();
    break;
  }
}

void ui_show_ringing(int8_t alarmIndex) {
  int8_t idx = (alarmIndex >= 0 && alarmIndex < NUM_ALARMS) ? alarmIndex : 0;
  // If a preview is running its stream simply becomes the alarm sound —
  // do not stop it here, main.cpp already saw audio_playing() == true.
  s_previewing = false;
  s_previewDeadline = 0;

  lv_label_set_text_fmt(s_rgTitle, "ALARM %d", idx + 1);
  const AlarmConfig &a = settings().alarms[idx];
  char hm[12];
  fmt_hm(hm, sizeof(hm), a.hour, a.minute);
  lv_label_set_text(s_rgTime, hm);

  // Snooze shame: rub this week's count in while deciding.
  uint16_t shame = alarm_snoozes_this_week();
  if (shame > 0)
    lv_label_set_text_fmt(s_rgHint,
                          "press = snooze (%ux this week!)   hold = stop",
                          (unsigned)shame);
  else
    lv_label_set_text_static(s_rgHint, "press = snooze    hold = stop");

  s_dimmed = false;
  display_set_contrast(settings().brightness); // full brightness while ringing
  load_screen(SCR_RING);
}

void ui_hide_ringing() {
  if (s_screen != SCR_RING)
    return;
  s_lastActMs = millis();
  load_screen(SCR_CLOCK);
}

void ui_poke() {
  s_lastActMs = millis();
  if (s_dimmed) {
    s_dimmed = false;
    display_set_contrast(settings().brightness);
  }
}

void ui_task() {
  uint32_t now = millis();

  if (s_previewing && s_previewDeadline &&
      (int32_t)(now - s_previewDeadline) >= 0)
    preview_stop();

  if (s_dirty && now - s_dirtyMs >= UI_SAVE_DEBOUNCE_MS) {
    s_dirty = false;
    settings_save();
  }

  if (s_screen != SCR_RING && !s_dimmed && settings().dimTimeoutS != 0 &&
      now - s_lastActMs >= (uint32_t)settings().dimTimeoutS * 1000ul) {
    s_dimmed = true;
    display_set_contrast(settings().dimBrightness);
  }

  if (now - s_lastRefreshMs < UI_REFRESH_MS)
    return;
  s_lastRefreshMs = now;

  switch (s_screen) {
  case SCR_CLOCK: refresh_clock(false); break;
  case SCR_TZ:    refresh_tz_info(false); break;
  case SCR_SYS:   refresh_sysinfo(false); break;
  case SCR_SKY:   refresh_sky(false); break;
  case SCR_PMIC:  refresh_pmic(false); break;
  default:        break;
  }
}
