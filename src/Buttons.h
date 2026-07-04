// Buttons.h — 4 tactile buttons in a row above the display (active low,
// external 1M pullups; enable internal pullups too for snappier edges).
// Debounced polling; emits short-press and long-press (>=700 ms) events
// through a small queue, and exposes raw held state for the UI/LVGL indev.
#pragma once
#include <Arduino.h>

#undef B1 // Arduino binary.h macro collides with the enumerator below

enum class BtnId : uint8_t { B1 = 0, B2, B3, B4 }; // left to right
enum class BtnAction : uint8_t { Short, Long };

struct ButtonEvent {
  BtnId id;
  BtnAction action;
};

void buttons_begin();
void buttons_task();                  // call every loop; does debouncing
bool buttons_pop_event(ButtonEvent &ev); // false when queue empty
bool buttons_is_down(BtnId id);       // debounced current state
bool buttons_any_down();
