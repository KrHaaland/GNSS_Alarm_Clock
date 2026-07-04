// TuneStorage.h — MX25L3233F 4MB QSPI flash as a FAT volume for alarm
// tunes, exposed over USB as a mass-storage drive (TinyUSB MSC) so tunes
// are uploaded by drag-and-drop. (The 24LC512 I2C EEPROM stays unused —
// the QSPI flash is bigger and faster, and fits sampled audio.)
//
// The volume is auto-formatted (FAT12) on first boot if blank.
// While the USB host holds the drive, storage_busy() is true and WAV
// playback from flash is refused (builtin melodies still work).
#pragma once
#include <Arduino.h>
#include <SdFat_Adafruit_Fork.h>

bool storage_begin();       // flash + FAT + USB MSC; false if flash missing
void storage_task();        // handle pending MSC writes / FS remount
bool storage_mounted();
bool storage_busy();        // USB host currently owns/writes the drive

FatVolume *storage_volume(); // nullptr when not mounted

// List *.wav files in the root directory. Returns count (<= maxNames).
// Each name is a bare 8.3/long filename, NUL-terminated, TUNE_NAME_LEN max.
uint8_t storage_list_tunes(char names[][32], uint8_t maxNames);
