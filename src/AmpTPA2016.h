// AmpTPA2016.h — TPA2016D2 class-D amp on I2C (0x58). The DAC (PA02) feeds
// the RIGHT channel input; the speaker hangs on OUTR (J6). ~SD is PB17
// (HIGH = enabled, has pullup). We enable only the right channel, configure
// the AGC as a limiter, and map a 0..10 volume to fixed gain.
#pragma once
#include <Arduino.h>

bool amp_begin();          // probes the chip; leaves it in shutdown
void amp_enable(bool on);  // drives ~SD + channel enable
void amp_set_volume(uint8_t vol0to10); // 0 mutes, 10 = max fixed gain
bool amp_present();
