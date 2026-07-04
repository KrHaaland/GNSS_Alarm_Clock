// Minimal USB-CDC diagnostic — isolates whether the core-bundled TinyUSB 3.1.0
// enumerates at all on this board (no storage, no LVGL, no I2C).
// Build/flash:  pio run -e diag  then copy the .uf2 to METROM4BOOT.
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

void setup() {
  Serial.begin(115200);
}

void loop() {
  static uint32_t t;
  if (millis() - t >= 500) {
    t = millis();
    Serial.print("diag alive t=");
    Serial.println(millis());
  }
}
