// AlarmManager.cpp — alarm trigger scan + Ringing/Snoozed state machine.
#include "AlarmManager.h"
#include "ClockKeeper.h"
#include "Settings.h"
#include "Timezone.h"

#define ALARM_EVAL_INTERVAL_MS 250u

static AlarmState s_state = AlarmState::Idle;
static int8_t s_ringingIndex = -1;
static uint32_t s_ringStartMs = 0;
static bool s_escalated = false;
static time_t s_snoozeUntil = 0;
// local epoch / 60 of the last minute each alarm matched; -1 = never.
// Set on any match (even while Ringing/Snoozed) so stopping during the
// trigger minute cannot refire the same alarm.
static int32_t s_lastFiredMinute[NUM_ALARMS];
static uint32_t s_lastEvalMs = 0;
static bool s_fireWasReRing = false;

static AlarmRingCb s_onRing = nullptr;
static AlarmSilenceCb s_onSilence = nullptr;

void alarm_begin() {
  s_state = AlarmState::Idle;
  s_ringingIndex = -1;
  s_ringStartMs = 0;
  s_escalated = false;
  s_snoozeUntil = 0;
  s_lastEvalMs = 0;
  for (uint8_t i = 0; i < NUM_ALARMS; i++)
    s_lastFiredMinute[i] = -1;
}

void alarm_set_callbacks(AlarmRingCb onRing, AlarmSilenceCb onSilence) {
  s_onRing = onRing;
  s_onSilence = onSilence;
}

AlarmState alarm_state() { return s_state; }

int8_t alarm_ringing_index() {
  return (s_state == AlarmState::Idle) ? -1 : s_ringingIndex;
}

time_t alarm_snooze_until() { return s_snoozeUntil; }

static void start_ringing(int8_t index, bool escalated) {
  s_state = AlarmState::Ringing;
  s_ringingIndex = index;
  s_ringStartMs = millis();
  s_escalated = false;
  if (s_onRing)
    s_onRing(index, escalated);
}

void alarm_task() {
  uint32_t nowMs = millis();
  if (nowMs - s_lastEvalMs < ALARM_EVAL_INTERVAL_MS)
    return;
  s_lastEvalMs = nowMs;

  if (clock_valid()) {
    time_t local = clock_now_local();
    int year, mon, day, hour, min, sec, wday;
    epoch_to_tm(local, year, mon, day, hour, min, sec, wday);
    int32_t curMinute = (int32_t)(local / 60);

    for (uint8_t i = 0; i < NUM_ALARMS; i++) {
      const AlarmConfig &a = settings().alarms[i];
      if (!a.enabled)
        continue;
      uint8_t mask = a.daysMask ? a.daysMask : 0x7F; // no days selected = daily
      if (hour != a.hour || min != a.minute || !((mask >> wday) & 1))
        continue;
      if (s_lastFiredMinute[i] == curMinute)
        continue;
      s_lastFiredMinute[i] = curMinute; // guard regardless of state
      // Ring when idle, or take over a snooze (a different alarm is now due —
      // don't let it be silently swallowed while another is snoozing).
      if (s_state == AlarmState::Idle || s_state == AlarmState::Snoozed) {
        s_fireWasReRing = false;
        start_ringing((int8_t)i, false);
      }
    }

    if (s_state == AlarmState::Snoozed && local >= s_snoozeUntil) {
      s_fireWasReRing = true;
      start_ringing(s_ringingIndex, false);
    }
  }

  // Ringing runs on millis so an alarm in progress escalates and
  // auto-stops even if the clock is lost mid-ring.
  if (s_state == AlarmState::Ringing) {
    uint32_t elapsed = nowMs - s_ringStartMs;
    uint8_t escMin = settings().buzzerAfterMin;
    if (!s_escalated && escMin > 0 && elapsed >= (uint32_t)escMin * 60000ul) {
      s_escalated = true;
      if (s_onRing)
        s_onRing(s_ringingIndex, true);
    }
    if (elapsed >= (uint32_t)ALARM_MAX_RING_MIN * 60000ul)
      alarm_stop();
  }
}

// Local epoch-day of the current week's Monday (weeks run Mon..Sun).
static uint32_t week_monday_day() {
  time_t lt = clock_now_local();
  int y, mo, d, h, mi, se, wd;
  epoch_to_tm(lt, y, mo, d, h, mi, se, wd); // wd: 0 = Sunday
  uint32_t today = (uint32_t)(lt / 86400);
  return today - (uint32_t)((wd + 6) % 7); // days since Monday
}

// The shame counter: every snooze is recorded, per week and all-time.
static void snooze_shame_bump() {
  Settings &st = settings();
  if (clock_valid()) {
    uint32_t monday = week_monday_day();
    if (st.snoozeWeekStart != monday) { // new week, fresh shame
      st.snoozeWeekStart = monday;
      st.snoozeWeek = 0;
    }
  }
  st.snoozeTotal++;
  st.snoozeWeek++;
  settings_save();
}

bool alarm_fire_was_rering() { return s_fireWasReRing; }

uint16_t alarm_snoozes_this_week() {
  const Settings &st = settings();
  if (clock_valid() && st.snoozeWeekStart != week_monday_day())
    return 0; // stored count is from an older week
  return st.snoozeWeek;
}

void alarm_snooze() {
  if (s_state != AlarmState::Ringing)
    return;
  s_state = AlarmState::Snoozed;
  s_snoozeUntil = clock_now_local() + (time_t)settings().snoozeMinutes * 60;
  snooze_shame_bump();
  if (s_onSilence)
    s_onSilence();
}

void alarm_stop() {
  if (s_state == AlarmState::Idle)
    return;
  s_state = AlarmState::Idle;
  s_ringingIndex = -1;
  s_escalated = false;
  if (s_onSilence)
    s_onSilence();
}

bool alarm_next_occurrence(time_t localNow, time_t &nextLocal, int8_t &index) {
  bool found = false;
  time_t best = 0;
  int8_t bestIdx = -1;

  int year, mon, day, hour, min, sec, wday;
  epoch_to_tm(localNow, year, mon, day, hour, min, sec, wday);
  // Local epoch is a pure civil-time line, so day steps are exactly 86400 s.
  time_t midnight = localNow - ((time_t)hour * 3600 + min * 60 + sec);

  for (uint8_t i = 0; i < NUM_ALARMS; i++) {
    const AlarmConfig &a = settings().alarms[i];
    if (!a.enabled)
      continue;
    uint8_t mask = a.daysMask ? a.daysMask : 0x7F; // no days selected = daily
    for (uint8_t d = 0; d < 8; d++) {
      uint8_t w = (uint8_t)((wday + d) % 7);
      if (!((mask >> w) & 1))
        continue;
      time_t t =
          midnight + (time_t)d * 86400 + (time_t)a.hour * 3600 + a.minute * 60;
      if (t < localNow)
        continue; // today's slot already passed
      if (!found || t < best) {
        found = true;
        best = t;
        bestIdx = (int8_t)i;
      }
      break; // first hit for this alarm is its soonest
    }
  }

  if (s_state == AlarmState::Snoozed && (!found || s_snoozeUntil < best)) {
    found = true;
    best = s_snoozeUntil;
    bestIdx = s_ringingIndex;
  }

  if (!found)
    return false;
  nextLocal = best;
  index = bestIdx;
  return true;
}
