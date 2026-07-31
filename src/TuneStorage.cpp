// TuneStorage.cpp — QSPI flash as a FAT volume, exposed over TinyUSB MSC.
// See TuneStorage.h for the contract.
//
// NOTE ON THE ACTUAL CHIP: the BOM specifies a Macronix MX25L3233F (4 MB),
// but the assembled boards carry a Winbond W25Q128 (JEDEC 0xEF4018, 16 MB) —
// an assembly substitution. flash.begin() is given the MX25L3233F descriptor
// first and falls back to the library's default device list (which includes
// the W25Q128), so the real size is detected either way and the USB drive
// correctly reports the chip's true capacity.

#include "TuneStorage.h"

#include <Adafruit_SPIFlash.h>
#include <Adafruit_TinyUSB.h>

// SdFat's FatFormatter refuses volumes <= 6MB, so a blank 4MB chip is
// formatted FAT12 with elm-chan's f_mkfs, exactly as the Adafruit SPIFlash
// SdFat_format example does. ff.c only exists inside that example folder;
// nothing else in the build compiles it, so pull it into this TU (quoted
// include resolves relative to this file).
#include "../.pio/libdeps/adafruit_metro_m4/Adafruit SPIFlash/examples/SdFat_format/ff.c"

static Adafruit_FlashTransport_QSPI flashTransport; // PB10/PB11 + PA08..11
static Adafruit_SPIFlash flash(&flashTransport);
static FatVolume fatfs;
static Adafruit_USBD_MSC usb_msc;

// Both 3V parts this footprint may carry (schematic symbol says MX25L3233F,
// assembled boards have had W25Q128JV). MX25L3233F is in flash_devices.h but
// not in the library's default scan list, so pass both explicitly. NB: the
// 1.8V W25Q128FW (JEDEC EF 60 18) is deliberately NOT here — it is out of
// spec on the 3.3V rail (see HARDWARE_V2.md); begin() fails and storage
// stays disabled rather than stressing a mis-fitted chip.
static const SPIFlash_Device_t tune_flash_devices[] = {MX25L3233F,
                                                       W25Q128JV_SQ};

static bool fs_mounted = false;
static volatile bool fs_changed = false;   // host wrote, remount pending
static volatile bool host_writing = false; // set in write cb, cleared on remount
static volatile uint32_t last_write_ms = 0;

// ---------------------------------------------------------------------------
// elm-chan FatFs diskio glue (used only by f_mkfs/f_mount during format)

extern "C" {

DSTATUS disk_status(BYTE pdrv) {
  (void)pdrv;
  return 0;
}

DSTATUS disk_initialize(BYTE pdrv) {
  (void)pdrv;
  return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count) {
  (void)pdrv;
  return flash.readBlocks(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count) {
  (void)pdrv;
  return flash.writeBlocks(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
  (void)pdrv;
  switch (cmd) {
  case CTRL_SYNC:
    flash.syncBlocks();
    return RES_OK;
  case GET_SECTOR_COUNT:
    *((DWORD *)buff) = flash.size() / 512;
    return RES_OK;
  case GET_SECTOR_SIZE:
    *((WORD *)buff) = 512;
    return RES_OK;
  case GET_BLOCK_SIZE:
    *((DWORD *)buff) = 8; // erase block size in sectors (4KB)
    return RES_OK;
  default:
    return RES_PARERR;
  }
}

} // extern "C"

// ---------------------------------------------------------------------------
// USB MSC callbacks (run in TinyUSB task context via yield(), not a hard ISR)

static int32_t msc_read_cb(uint32_t lba, void *buffer, uint32_t bufsize) {
  // SPIFlash block API has 4KB sector caching internally
  return flash.readBlocks(lba, (uint8_t *)buffer, bufsize / 512)
             ? (int32_t)bufsize
             : -1;
}

static int32_t msc_write_cb(uint32_t lba, uint8_t *buffer, uint32_t bufsize) {
  host_writing = true;
  last_write_ms = millis();
  return flash.writeBlocks(lba, buffer, bufsize / 512) ? (int32_t)bufsize : -1;
}

static void msc_flush_cb(void) {
  flash.syncBlocks();
  fatfs.cacheClear(); // force FS re-read after host writes
  fs_changed = true;
  last_write_ms = millis();
}

// ---------------------------------------------------------------------------

static bool format_fat12(void) {
  // f_mkfs picks FAT12 for this cluster count; FM_FAT keeps an MBR so the
  // volume is mountable by SdFat (part auto-detect) and desktop OSes alike.
  uint8_t workbuf[4096];
  FATFS elmchanFatfs;

  if (f_mkfs("", FM_FAT, 0, workbuf, sizeof(workbuf)) != FR_OK)
    return false;
  if (f_mount(&elmchanFatfs, "0:", 1) == FR_OK) {
    f_setlabel("TUNES");
    f_unmount("0:");
  }
  return flash.syncBlocks();
}

static void write_readme(void) {
  File32 f = fatfs.open("README.TXT", O_WRONLY | O_CREAT | O_TRUNC);
  if (!f)
    return;
  f.print("GNSS Alarm Clock - tune storage\r\n"
          "Drop 8/16-bit PCM WAV files in this folder to use them as alarm "
          "tunes.\r\n"
          "Filenames up to 31 characters. Pick the tune in the alarm menu.\r\n");
  f.close();
  flash.syncBlocks();
}

bool storage_begin(void) {
  if (!flash.begin(tune_flash_devices, 2))
    return false;

  // SCSI INQUIRY identity (what Windows shows for the disk device):
  // vendor max 8 chars, product max 16. "KH" + "GNSS Alarm Clock" reads as
  // "KH GNSS Alarm Clock USB Device" in Device Manager.
  usb_msc.setID("KH", "GNSS Alarm Clock", "1.0");
  usb_msc.setReadWriteCallback(msc_read_cb, msc_write_cb, msc_flush_cb);
  usb_msc.setCapacity(flash.size() / 512, 512);
  usb_msc.setUnitReady(true);
  usb_msc.begin(); // must run before host enumerates; called early in setup

  fs_mounted = fatfs.begin(&flash);
  if (!fs_mounted && format_fat12()) {
    fs_mounted = fatfs.begin(&flash);
    if (fs_mounted)
      write_readme();
  }
  return true; // flash present; mount state via storage_mounted()
}

void storage_task(void) {
  if (fs_changed && (uint32_t)(millis() - last_write_ms) >= 500) {
    fs_changed = false; // a write during remount re-arms the debounce
    fs_mounted = fatfs.begin(&flash);
    host_writing = false;
  }
}

bool storage_mounted(void) { return fs_mounted; }

bool storage_busy(void) { return host_writing || fs_changed; }

FatVolume *storage_volume(void) { return fs_mounted ? &fatfs : nullptr; }

uint8_t storage_list_tunes(char names[][32], uint8_t maxNames) {
  if (!fs_mounted || maxNames == 0)
    return 0;

  FatFile root;
  if (!root.openRoot(&fatfs))
    return 0;

  uint8_t count = 0;
  FatFile file;
  char name[64];
  // Hard iteration cap: a FAT12 root dir holds at most 512 entries, but a
  // corrupt/unstably-read directory chain (flaky QSPI solder, torn write)
  // can loop forever in openNext() and freeze the UI on the Tunes screen.
  uint16_t iter = 0;
  while (count < maxNames && iter++ < 512 && file.openNext(&root, O_RDONLY)) {
    bool skip = file.isDir() || file.isHidden() || file.isSystem();
    size_t len = file.getName(name, sizeof(name));
    file.close();
    if (skip || len == 0 || len > 31)
      continue;
    if (name[0] == '.') // macOS ._AppleDouble & co (hidden attr not set)
      continue;
    if (len < 5 || strcasecmp(name + len - 4, ".wav") != 0)
      continue;
    memcpy(names[count], name, len + 1);
    count++;
  }
  root.close();
  return count;
}
