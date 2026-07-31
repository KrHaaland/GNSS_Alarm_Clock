// main_ledtest.cpp — board bring-up diagnostic (env:ledtest): blinks the
// three LED sections sequentially and scans the whole I2C bus every cycle,
// reporting over USB serial. No display or SWD needed — made for checking a
// freshly soldered board before the THT parts go on.
//
// Pin map assumed identical to v1 (Metro M4 compatible):
//   D8 = LEDSL-S (left, 10 LEDs), D9 = LEDSB-S (bottom, 14),
//   A2 = LEDSR-S (right, 10) — low-side MOSFET gates, HIGH = on.
//   LED anodes hang on ALARMPOWER: that rail must be up for light to show.
#include <Adafruit_FlashTransport.h>
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
  case 0x18: return "LIS3DH accelerometer (SA0 low)";
  case 0x19: return "LIS3DH accelerometer (SA0 floated high)";
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

// --- GNSS listener: is the L86 saying anything at all? ---------------------
// Counts raw bytes from Serial1 (9600 NMEA) and keeps the last complete
// sentence. The L86 factory default emits RMC/VTG/GGA/GSA/GSV at 1 Hz with
// no configuration, so a healthy module shows traffic within seconds.
static uint8_t s_jedec[4]; // QSPI flash JEDEC ID, probed once in setup()
static uint32_t gnssChars = 0;
static char gnssLine[100];
static uint8_t gnssLen = 0;
static char gnssLast[100] = "(nothing yet)";

static void gnss_drain() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    gnssChars++;
    if (c == '$')
      gnssLen = 0;
    if (c == '\r' || c == '\n') {
      if (gnssLen > 5) {
        memcpy(gnssLast, gnssLine, gnssLen);
        gnssLast[gnssLen] = '\0';
      }
      gnssLen = 0;
    } else if (gnssLen < sizeof(gnssLine) - 1) {
      gnssLine[gnssLen++] = c;
    }
  }
}

static void delay_drain(uint32_t ms) { // delay without dropping UART bytes
  uint32_t t0 = millis();
  while (millis() - t0 < ms)
    gnss_drain();
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
      Serial.print(known_name(a));
      if (a == 0x18 || a == 0x19) { // verify it IS a LIS3DH: WHO_AM_I == 0x33
        Wire.beginTransmission(a);
        Wire.write(0x0F);
        if (Wire.endTransmission(false) == 0 &&
            Wire.requestFrom(a, (uint8_t)1) == 1) {
          uint8_t who = Wire.read();
          Serial.print(who == 0x33 ? "  [WHO_AM_I OK]" : "  [WHO_AM_I BAD: 0x");
          if (who != 0x33)
            Serial.print(who, HEX), Serial.print("]");
        }
      }
      Serial.println();
      found++;
    }
  }
  if (!found)
    Serial.println("  NO devices ACKed — check SDA/SCL, pullups, 3V3 rail");
  Serial.print("PA04 (CAPGOOD/charger status): ");
  Serial.println(digitalRead(PIN_CAPGOOD) ? "HIGH" : "LOW");
  Serial.print("GNSS: ");
  Serial.print(gnssChars);
  Serial.print(" bytes total, last: ");
  Serial.println(gnssLast);
  Serial.print("QSPI JEDEC: ");
  for (uint8_t i = 0; i < 3; i++) {
    if (s_jedec[i] < 0x10)
      Serial.print('0');
    Serial.print(s_jedec[i], HEX);
    Serial.print(' ');
  }
  // EF 40 18 = W25Q128JV-SQ (3V, lib OK); EF 60 18 = W25Q128FW (1.8V PART -
  // WRONG for our 3.3V rail); C2 20 16 = MX25L3233F; 00/FF = no answer.
  Serial.println();
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
  Serial1.begin(9600); // L86 NMEA

  // L86 aliveness test: pulse RESET_N (PB31, direct PORT — not in the
  // Metro variant) low 100 ms, then release to the module's internal
  // pull-up. A healthy, powered L86 emits a $PMTK011 boot burst within
  // ~1 s of reset even with zero antenna signal — silence after this
  // means power/solder/TX-path, not RF.
  PORT->Group[1].OUTCLR.reg = (1ul << 31);
  PORT->Group[1].DIRSET.reg = (1ul << 31);
  delay(100);
  PORT->Group[1].DIRCLR.reg = (1ul << 31); // release, hi-Z
  PORT->Group[1].PINCFG[31].reg = 0;

  Serial.println("\n=== GNSS Alarm Clock board bring-up (ledtest) ===");
  Serial.println("L86 RESET_N pulsed - watch for $PMTK011 boot burst");

  // QSPI flash identity: raw JEDEC-ID (0x9F) probe. Healthy chips:
  //   EF 40 18 = W25Q128JV-SQ (in the library's builtin list)
  //   EF 70 18 = W25Q128JV-IM/PM (NOT in the builtin list!)
  //   C2 20 16 = MX25L3233F (v1 part, our explicit descriptor)
  //   00/FF    = no answer -> solder/QSPI-path problem
  static Adafruit_FlashTransport_QSPI qspiTransport;
  qspiTransport.begin();
  qspiTransport.readCommand(0x9F, s_jedec, 4);

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
    delay_drain(1500);
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
    delay_drain(500);
  }
  delay_drain(1000);
}
