// Buttons.cpp — debounced polling of the 4 face buttons, short/long events.
#include "Buttons.h"
#include "pins.h"

#define NUM_BUTTONS 4
#define QUEUE_SIZE 8 // power of two
#define DEBOUNCE_MS 25
#define LONG_PRESS_MS 700

static const uint8_t kPins[NUM_BUTTONS] = {PIN_BUTTON1, PIN_BUTTON2,
                                           PIN_BUTTON3, PIN_BUTTON4};

struct BtnState {
  bool stable;   // debounced pressed (active-low already resolved)
  bool lastRaw;
  uint32_t lastEdgeMs;
  uint32_t pressMs;
  bool longFired;
};
static BtnState st[NUM_BUTTONS];

static ButtonEvent queue[QUEUE_SIZE];
static uint8_t qHead = 0;
static uint8_t qCount = 0;

static void push_event(BtnId id, BtnAction action) {
  if (qCount == QUEUE_SIZE) { // drop oldest
    qHead = (qHead + 1) & (QUEUE_SIZE - 1);
    qCount--;
  }
  uint8_t tail = (qHead + qCount) & (QUEUE_SIZE - 1);
  queue[tail].id = id;
  queue[tail].action = action;
  qCount++;
}

void buttons_begin() {
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    // 1M external pullups are slow; internal ~40k pullup sharpens edges
    pinMode(kPins[i], INPUT_PULLUP);
    st[i].stable = false;
    st[i].lastRaw = false;
    st[i].lastEdgeMs = 0;
    st[i].pressMs = 0;
    st[i].longFired = false;
  }
  qHead = 0;
  qCount = 0;
}

void buttons_task() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
    bool raw = (digitalRead(kPins[i]) == LOW);
    if (raw != st[i].lastRaw) {
      st[i].lastRaw = raw;
      st[i].lastEdgeMs = now;
    }
    if (raw != st[i].stable && (uint32_t)(now - st[i].lastEdgeMs) >= DEBOUNCE_MS) {
      st[i].stable = raw;
      if (raw) {
        st[i].pressMs = now;
        st[i].longFired = false;
      } else if (!st[i].longFired) { // Long already emitted -> swallow release
        push_event((BtnId)i, BtnAction::Short);
      }
    }
    if (st[i].stable && !st[i].longFired &&
        (uint32_t)(now - st[i].pressMs) >= LONG_PRESS_MS) {
      st[i].longFired = true;
      push_event((BtnId)i, BtnAction::Long);
    }
  }
}

bool buttons_pop_event(ButtonEvent &ev) {
  if (qCount == 0)
    return false;
  ev = queue[qHead];
  qHead = (qHead + 1) & (QUEUE_SIZE - 1);
  qCount--;
  return true;
}

bool buttons_is_down(BtnId id) { return st[(uint8_t)id].stable; }

bool buttons_any_down() {
  for (uint8_t i = 0; i < NUM_BUTTONS; i++)
    if (st[i].stable)
      return true;
  return false;
}
