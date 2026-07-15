// AlarmManager.h — alarm scheduling and the ringing state machine.
//
// Triggering: an enabled alarm fires when local time reaches HH:MM on an
// enabled weekday (daysMask bit0=Sun..bit6=Sat), edge-triggered per minute
// (won't refire within the same minute, nor after snooze/stop games).
//
// Ringing orchestration (via callbacks into main): LED chase + tune.
// Escalation: after settings().buzzerAfterMin minutes of unacknowledged
// ringing, the supercap power buzzer joins in. Auto-silence after
// ALARM_MAX_RING_MIN, re-arming for the next day.
// Snooze: short press (or accelerometer tap) = snooze; long press = stop.
#pragma once
#include <Arduino.h>
#include <time.h>

#define ALARM_MAX_RING_MIN 30

enum class AlarmState : uint8_t { Idle, Ringing, Snoozed };

void alarm_begin();
void alarm_task(); // evaluate triggers; call every loop (uses ClockKeeper)

AlarmState alarm_state();
int8_t alarm_ringing_index();  // which alarm rings/snoozes, -1 if none
void alarm_snooze();           // Ringing -> Snoozed (settings().snoozeMinutes)
void alarm_stop();             // Ringing/Snoozed -> Idle (until next match)
time_t alarm_snooze_until();   // local epoch, valid while Snoozed

// Next scheduled alarm occurrence in local time; false if none enabled.
bool alarm_next_occurrence(time_t localNow, time_t &nextLocal, int8_t &index);

// True when the most recent ring was a snooze re-ring (used to skip the
// gentle-wake volume ramp — you are already awake).
bool alarm_fire_was_rering();

// Snooze shame counter: snoozes so far in the current local week (Mon-start).
// Week-rollover aware; the all-time count lives in settings().snoozeTotal.
uint16_t alarm_snoozes_this_week();

// main.cpp registers these to start/stop the show (LEDs, audio, amp).
typedef void (*AlarmRingCb)(int8_t alarmIndex, bool escalated);
typedef void (*AlarmSilenceCb)();
void alarm_set_callbacks(AlarmRingCb onRing, AlarmSilenceCb onSilence);
