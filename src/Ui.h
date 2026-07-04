// Ui.h — LVGL user interface: clock face + menu system driven by the 4
// buttons above the display (left to right):
//   B1 = Back / open menu (long-press on clock = nothing special)
//   B2 = Prev / Up        B3 = Next / Down        B4 = OK / Enter
// While an alarm rings, buttons are captured by main.cpp (snooze/stop) —
// the UI only shows the ringing screen.
//
// Screens:
//   Clock      — big HH:MM(:SS), date, next-alarm line, status bar
//                (GNSS sats/fix, supercap, USB, sync source, DST)
//   Menu       — Alarms / Time & Zone / Display / Tunes / System info
//   AlarmEdit  — per alarm: enable, hour, minute, days, tune, test
//   TimeZone   — auto (GNSS) on/off, manual zone pick, force sync, 12/24h
//   Display    — brightness, dim timeout
//   Tunes      — list WAVs from flash + builtin melodies, preview
//   SysInfo    — fix, sats, HDOP, lat/lon, tz, last sync, cap status, FW
//   Ringing    — which alarm + big time + "snooze/stop" hint
//
// Input: main.cpp routes ButtonEvents here via ui_handle_event() (an LVGL
// keypad indev drains an internal key queue). While an alarm rings, main
// intercepts the events instead, so they never reach the UI.
#pragma once
#include <Arduino.h>
#include "Buttons.h"

void ui_begin();          // build screens; requires display_init() done
void ui_task();           // per-loop: refresh dynamic labels, dim handling
void ui_handle_event(const ButtonEvent &ev); // navigation input
void ui_show_ringing(int8_t alarmIndex);
void ui_hide_ringing();
// Wake display / reset dim timer (called on any button event).
void ui_poke();
