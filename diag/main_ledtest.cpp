// main_ledtest.cpp — board bring-up diagnostic (env:ledtest): blinks the
// three LED sections sequentially and scans the whole I2C bus every cycle,
// reporting over USB serial. No display or SWD needed — made for checking a
// freshly soldered board before the THT parts go on.
//
// Pin map assumed identical to v1 (Metro M4 compatible):
//   D8 = LEDSL-S (left, 10 LEDs), D9 = LEDSB-S (bottom, 14),
//   A2 = LEDSR-S (right, 10) — low-side MOSFET gates, HIGH = on.
//   LED anodes hang on ALARMPOWER: that rail must be up for light to show.
#include <Arduino.h>
#include <Wire.h>

#define PIN_LEDS_LEFT 8
#define PIN_LEDS_BOTTOM 9
#define PIN_LEDS_RIGHT A2
#define PIN_CAPGOOD A3 // v1: LTC3226 CAPGD; v2: charger status, if routed

// BOTTOM (14 LEDs, the heaviest load) goes LAST: on a board without the
// battery fitted it collapses the rail and resets the MCU, and the sections
// before it must get their verdict in first.
static const uint8_t LED_PINS[3] = {PIN_LEDS_LEFT, PIN_LEDS_RIGHT,
                                    PIN_LEDS_BOTTOM};
static const char *const LED_NAMES[3] = {"LEFT (D8/PA21)", "RIGHT (A2/PA06)",
                                         "BOTTOM (D9/PA20)"};

static const char *known_name(uint8_t a) {
  switch (a) {
  case 0x18: return "LIS3DH accelerometer";
  case 0x50: return "24LC512 EEPROM (v1 only — should be absent on v2)";
  case 0x52: return "RV-3028 RTC";
  case 0x58: return "TPA2016 amp";
  case 0x6B: return "nPM1300 PMIC (v2)";
  default:   return "UNKNOWN device (new on this board?)";
  }
}

// --- nPM1300 (I2C 0x6B, 16-bit register addressing) ------------------------
// The PMIC holds its VBUS input current limit at 100 mA after power-up until
// the host raises it — which is exactly what browns the board out when the
// bottom LED section (the heaviest load, no battery fitted) switches on.
#define NPM_ADDR 0x6B
#define NPM_TASKUPDATEILIMSW 0x0200 // write 1: apply VBUSINILIM0
#define NPM_VBUSINILIM0 0x0201      // 1 = 100 mA (default), 5 = 500 mA
#define NPM_VBUSINSTATUS 0x0207

static bool npm_write(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(NPM_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static int npm_read(uint16_t reg) {
  Wire.beginTransmission(NPM_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0)
    return -1;
  if (Wire.requestFrom((uint8_t)NPM_ADDR, (uint8_t)1) != 1)
    return -1;
  return Wire.read();
}

static void npm_raise_vbus_limit() {
  int before = npm_read(NPM_VBUSINILIM0);
  bool ok = npm_write(NPM_VBUSINILIM0, 5) && // 500 mA
            npm_write(NPM_TASKUPDATEILIMSW, 1);
  Serial.print("nPM1300 VBUS ILIM: ");
  Serial.print(before);
  Serial.print(" -> ");
  Serial.print(npm_read(NPM_VBUSINILIM0));
  Serial.print(" (x100 mA), apply ");
  Serial.print(ok ? "OK" : "FAILED");
  Serial.print(", VBUSINSTATUS=0x");
  Serial.println(npm_read(NPM_VBUSINSTATUS), HEX);
}

static void i2c_scan() {
  Serial.println("--- I2C scan 0x08..0x77 (100 kHz) ---");
  uint8_t found = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print("  ACK 0x");
      if (a < 0x10)
        Serial.print('0');
      Serial.print(a, HEX);
      Serial.print("  ");
      Serial.println(known_name(a));
      found++;
    }
  }
  if (!found)
    Serial.println("  NO devices ACKed — check SDA/SCL, pullups, 3V3 rail");
  Serial.print("PA04 (CAPGOOD/charger status): ");
  Serial.println(digitalRead(PIN_CAPGOOD) ? "HIGH" : "LOW");
}

void setup() {
  for (uint8_t i = 0; i < 3; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW);
  }
  pinMode(PIN_CAPGOOD, INPUT);
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) {
  } // wait briefly for the host to open the port
  Wire.begin();
  Wire.setClock(100000);
  Serial.println("\n=== GNSS Alarm Clock board bring-up (ledtest) ===");

  Wire.beginTransmission(NPM_ADDR);
  if (Wire.endTransmission() == 0)
    npm_raise_vbus_limit(); // v2: unlock 500 mA before any LED load
}

void loop() {
  static uint32_t cycle = 0;
  static bool ledsArmed = false;
  Serial.print("\n===== cycle ");
  Serial.print(cycle++);
  Serial.println(" =====");
  i2c_scan();
  if (!ledsArmed) {
    Serial.println("(scan-only mode - send any character to arm the LED test)");
    Serial.flush();
    if (Serial.available()) {
      while (Serial.available())
        Serial.read();
      ledsArmed = true;
    }
    delay(1500);
    return;
  }

  // Short pulses: enough to see, low enough average load that a weak rail
  // (no battery fitted yet) hopefully survives all three sections.
  for (uint8_t i = 0; i < 3; i++) {
    Serial.print("LED section ON: ");
    Serial.println(LED_NAMES[i]);
    Serial.flush();
    digitalWrite(LED_PINS[i], HIGH);
    delay(150);
    digitalWrite(LED_PINS[i], LOW);
    Serial.println("  ...survived");
    Serial.flush();
    delay(500);
  }
  delay(1000);
}
