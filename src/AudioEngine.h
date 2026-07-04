// AudioEngine.h — DAC audio playback for alarm tunes.
//
// Pipeline: TC2 timer ISR at the sample rate pops samples from a lock-free
// ring buffer and writes DAC0 (PA02, 12-bit) -> AC coupled -> TPA2016D2 ->
// speaker (J6). The ring is refilled from loop() context by audio_task():
//  - WAV files streamed from the QSPI flash FAT volume (PCM u8/s16,
//    mono/stereo downmixed, 8..48 kHz — timer runs at the file's rate)
//  - builtin synthesized melodies (sine with decay envelope, note tables)
//    when no file is selected/available.
//
// The power buzzer (PB16 -> MOSFET -> J5 on the supercap rail) is driven
// with tone()/noTone() (TC0) as a louder escalation stage.
//
// Volume: TPA2016 fixed gain (via AmpTPA2016) plus digital scaling here.
#pragma once
#include <Arduino.h>

#define AUDIO_MELODY_COUNT 3
extern const char *const AUDIO_MELODY_NAMES[AUDIO_MELODY_COUNT];

void audio_begin();  // DAC setup; timer stays off until playback
void audio_task();   // refill ring buffer / advance melody; call every loop

// Start a WAV from the tune volume, e.g. "sunrise.wav" (root of the flash
// drive). Returns false if the file is missing/unsupported (caller should
// fall back to a melody).
bool audio_play_wav(const char *filename, bool loop = true);
void audio_play_melody(uint8_t id, bool loop = true);
void audio_stop();       // stops DAC stream (does not touch the amp)
bool audio_playing();

void audio_set_volume(uint8_t vol0to10); // digital part of volume control

void buzzer_start(uint16_t freqHz = 2400); // escalation buzzer on/off
void buzzer_stop();
