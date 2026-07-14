// AmpTPA2016.h — TPA2016D2 class-D amp on I2C (0x58). The DAC (PA02) feeds
// the RIGHT channel input; the speaker hangs on OUTR (J6). ~SD is PB17
// (HIGH = enabled, has pullup).
//
// Loudness architecture (see docs/adr/0010): AGC compression 1:4 levels out
// differently-mastered tunes; with AGC active the OUTPUT LIMITER is the real
// volume knob, so volume 0..10 maps to the limiter level (0.5 dB steps) and
// the fixed gain stays constant. The gentle-wake ramp walks the limiter up.
#pragma once
#include <Arduino.h>

bool amp_begin();          // probes the chip; leaves it in shutdown
void amp_enable(bool on);  // drives ~SD + channel enable
void amp_set_volume(uint8_t vol0to10); // 0 mutes (SWS); cancels any ramp
bool amp_present();

// Gentle wake: start at the quietest limiter level and ramp to `vol0to10`
// over `seconds`. seconds==0 -> jump directly. amp_task() advances the ramp.
void amp_ramp_to(uint8_t vol0to10, uint16_t seconds);
void amp_task();

// Live status while enabled: false in shutdown / chip absent.
// faultOut = over-current on an output, thermal = over-temperature shutdown.
bool amp_enabled();
bool amp_get_status(bool &faultOut, bool &thermal);
