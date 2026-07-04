// AudioEngine.cpp — TC2-paced DAC playback: WAV streaming from the QSPI
// tune volume plus builtin synthesized melodies. See AudioEngine.h.
#include "AudioEngine.h"
#include "TuneStorage.h"
#include "pins.h"

const char *const AUDIO_MELODY_NAMES[AUDIO_MELODY_COUNT] = {
    "Sunrise", "Classic beep", "Chime"};

// ---------------------------------------------------------------- ring ---
#define RING_SIZE 4096u
#define RING_MASK (RING_SIZE - 1u)
static volatile uint16_t ring[RING_SIZE]; // ready-to-play 12-bit samples
static volatile uint16_t rHead;           // ISR pops here
static volatile uint16_t rTail;           // audio_task pushes here

enum SrcState { SRC_IDLE, SRC_WAV, SRC_MELODY, SRC_DRAIN };
static SrcState srcState = SRC_IDLE;
static bool loopFlag;
static bool dacReady; // audio_begin ran

// Digital volume, Q8 (256 = unity). Exponential-ish steps, ~3 dB apart.
static const uint16_t VOL_TABLE[11] = {0,   16,  24,  34,  48, 68,
                                       96,  128, 165, 208, 256};
static uint16_t volFactor = 165; // default vol 8

// ------------------------------------------------------------ TC2 timer ---
// GCLK0 = 120 MHz, prescaler /8 -> 15 MHz count clock; MFRQ overflows at CC0.
#define TC2_BASE_HZ 15000000ul

static void tc2_sync_enable() {
  while (TC2->COUNT16.SYNCBUSY.bit.ENABLE) {
  }
}

static void tc2_init() {
  MCLK->APBBMASK.bit.TC2_ = 1;
  GCLK->PCHCTRL[TC2_GCLK_ID].reg = GCLK_PCHCTRL_GEN_GCLK0 | GCLK_PCHCTRL_CHEN;
  while (!(GCLK->PCHCTRL[TC2_GCLK_ID].reg & GCLK_PCHCTRL_CHEN)) {
  }
  TC2->COUNT16.CTRLA.bit.ENABLE = 0;
  tc2_sync_enable();
  TC2->COUNT16.CTRLA.bit.SWRST = 1;
  while (TC2->COUNT16.SYNCBUSY.bit.SWRST || TC2->COUNT16.CTRLA.bit.SWRST) {
  }
  TC2->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16 | TC_CTRLA_PRESCALER_DIV8;
  TC2->COUNT16.WAVE.reg = TC_WAVE_WAVEGEN_MFRQ;
  TC2->COUNT16.INTENSET.reg = TC_INTENSET_OVF;
  NVIC_SetPriority(TC2_IRQn, 1);
  NVIC_ClearPendingIRQ(TC2_IRQn);
  NVIC_EnableIRQ(TC2_IRQn);
}

static void tc2_start(uint32_t sampleRate) {
  uint32_t top = TC2_BASE_HZ / sampleRate;
  if (top > 0)
    top--;
  if (top > 65535ul)
    top = 65535ul; // < 229 Hz never requested, safety clamp
  TC2->COUNT16.CTRLA.bit.ENABLE = 0;
  tc2_sync_enable();
  TC2->COUNT16.CC[0].reg = (uint16_t)top;
  while (TC2->COUNT16.SYNCBUSY.bit.CC0) {
  }
  TC2->COUNT16.COUNT.reg = 0;
  while (TC2->COUNT16.SYNCBUSY.bit.COUNT) {
  }
  TC2->COUNT16.INTFLAG.reg = TC_INTFLAG_OVF;
  TC2->COUNT16.CTRLA.bit.ENABLE = 1;
  tc2_sync_enable();
}

static void tc2_stop() {
  TC2->COUNT16.CTRLA.bit.ENABLE = 0;
  tc2_sync_enable();
}

void TC2_Handler() {
  TC2->COUNT16.INTFLAG.reg = TC_INTFLAG_OVF;
  uint16_t h = rHead;
  if (h == rTail) {
    DAC->DATA[0].reg = 2048; // underrun: hold midpoint
  } else {
    // Previous conversion finished long ago at <=48 kHz; plain write is safe.
    DAC->DATA[0].reg = ring[h];
    rHead = (h + 1) & RING_MASK;
  }
}

static uint32_t ring_free() {
  return ((uint32_t)rHead - rTail - 1u) & RING_MASK;
}

static void ring_push(uint16_t v) {
  uint16_t t = rTail;
  ring[t] = v;
  rTail = (t + 1) & RING_MASK; // volatile store orders after sample store
}

static uint16_t apply_volume(int32_t s12) {
  return (uint16_t)(2048 + (((s12 - 2048) * (int32_t)volFactor) >> 8));
}

// ------------------------------------------------------------ WAV source ---
static File32 wavFile;
static uint32_t wavDataStart;
static uint32_t wavDataSize;
static uint32_t wavBytesLeft;
static uint8_t wavChannels;
static uint8_t wavBytesPerSample; // per channel: 1 or 2
static uint8_t wavFrameBytes;
static uint8_t fileBuf[512];

static uint32_t rd_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Parses RIFF/fmt/data. On success fills the wav* globals (file left
// positioned at data start) and returns the sample rate; 0 on failure.
static uint32_t wav_parse(File32 &f) {
  uint8_t hdr[12];
  if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
      memcmp(hdr + 8, "WAVE", 4) != 0)
    return 0;

  bool haveFmt = false;
  uint32_t rate = 0;
  for (uint8_t i = 0; i < 64; i++) {
    uint8_t ch[8];
    if (f.read(ch, 8) != 8)
      return 0;
    uint32_t sz = rd_u32(ch + 4);
    if (memcmp(ch, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      if (sz < 16 || f.read(fmt, 16) != 16)
        return 0;
      uint16_t audioFormat = rd_u16(fmt + 0);
      uint16_t channels = rd_u16(fmt + 2);
      rate = rd_u32(fmt + 4);
      uint16_t bits = rd_u16(fmt + 14);
      if (audioFormat != 1 || (channels != 1 && channels != 2) ||
          (bits != 8 && bits != 16) || rate < 8000 || rate > 48000)
        return 0;
      wavChannels = (uint8_t)channels;
      wavBytesPerSample = (uint8_t)(bits / 8);
      wavFrameBytes = (uint8_t)(wavChannels * wavBytesPerSample);
      haveFmt = true;
      if (sz > 16 && !f.seekCur((int32_t)(sz - 16 + (sz & 1))))
        return 0;
    } else if (memcmp(ch, "data", 4) == 0) {
      if (!haveFmt)
        return 0;
      wavDataStart = f.curPosition();
      uint32_t avail = f.fileSize() - wavDataStart;
      if (sz > avail)
        sz = avail; // tolerate truncated files
      wavDataSize = sz - (sz % wavFrameBytes);
      return wavDataSize ? rate : 0;
    } else {
      if (!f.seekCur((int32_t)(sz + (sz & 1))))
        return 0;
    }
  }
  return 0;
}

static void wav_fill() {
  if (!storage_mounted() || storage_busy()) {
    audio_stop(); // USB host owns the drive: abandon playback
    return;
  }
  for (;;) {
    uint32_t freeSlots = ring_free();
    if (freeSlots < 256)
      return;
    if (wavBytesLeft == 0) {
      if (loopFlag && wavFile.seekSet(wavDataStart)) {
        wavBytesLeft = wavDataSize;
      } else {
        srcState = SRC_DRAIN;
        return;
      }
    }
    uint32_t want = freeSlots * wavFrameBytes;
    if (want > sizeof(fileBuf))
      want = sizeof(fileBuf);
    if (want > wavBytesLeft)
      want = wavBytesLeft;
    want -= want % wavFrameBytes;
    if (want == 0) {
      wavBytesLeft = 0;
      continue;
    }
    int n = wavFile.read(fileBuf, want);
    if (n <= 0) {
      srcState = SRC_DRAIN;
      return;
    }
    wavBytesLeft -= (uint32_t)n;
    n -= n % wavFrameBytes;
    for (int i = 0; i < n; i += wavFrameBytes) {
      int32_t s12;
      if (wavBytesPerSample == 1) {
        uint16_t s = fileBuf[i];
        if (wavChannels == 2)
          s = (uint16_t)((s + fileBuf[i + 1]) >> 1);
        s12 = (int32_t)s << 4;
      } else {
        int32_t s = (int16_t)rd_u16(fileBuf + i);
        if (wavChannels == 2)
          s = (s + (int16_t)rd_u16(fileBuf + i + 2)) >> 1;
        s12 = (s >> 4) + 2048;
      }
      ring_push(apply_volume(s12));
    }
  }
}

// --------------------------------------------------------- melody source ---
#define MELODY_RATE 22050ul
#define GAP_SAMPLES (MELODY_RATE * 20ul / 1000ul) // 20 ms between notes

struct Note {
  uint16_t freqHz; // 0 = rest
  uint16_t ms;
};

static const Note MEL_SUNRISE[] = {
    // gentle ascending C major arpeggio, looping
    {262, 350}, {0, 150}, {330, 350}, {0, 150}, {392, 350}, {0, 150},
    {523, 350}, {0, 150}, {659, 350}, {0, 150}, {784, 550}, {0, 700},
};
static const Note MEL_BEEP[] = {
    {880, 120}, {0, 80}, {880, 120}, {0, 80},
    {880, 120}, {0, 80}, {880, 120}, {0, 580},
};
static const Note MEL_CHIME[] = {
    // Westminster quarters approximation: E4 C4 D4 G3 / G3 D4 E4 C4
    {330, 600}, {262, 600}, {294, 600}, {196, 1000}, {0, 400},
    {196, 600}, {294, 600}, {330, 600}, {262, 1000}, {0, 1500},
};

static const Note *const MELODIES[AUDIO_MELODY_COUNT] = {MEL_SUNRISE, MEL_BEEP,
                                                         MEL_CHIME};
static const uint8_t MELODY_LEN[AUDIO_MELODY_COUNT] = {
    sizeof(MEL_SUNRISE) / sizeof(Note), sizeof(MEL_BEEP) / sizeof(Note),
    sizeof(MEL_CHIME) / sizeof(Note)};

static const int8_t SINE_LUT[256] = {
       0,    3,    6,    9,   12,   16,   19,   22,   25,   28,   31,   34,   37,   40,   43,   46,
      49,   51,   54,   57,   60,   63,   65,   68,   71,   73,   76,   78,   81,   83,   85,   88,
      90,   92,   94,   96,   98,  100,  102,  104,  106,  107,  109,  111,  112,  113,  115,  116,
     117,  118,  120,  121,  122,  122,  123,  124,  125,  125,  126,  126,  126,  127,  127,  127,
     127,  127,  127,  127,  126,  126,  126,  125,  125,  124,  123,  122,  122,  121,  120,  118,
     117,  116,  115,  113,  112,  111,  109,  107,  106,  104,  102,  100,   98,   96,   94,   92,
      90,   88,   85,   83,   81,   78,   76,   73,   71,   68,   65,   63,   60,   57,   54,   51,
      49,   46,   43,   40,   37,   34,   31,   28,   25,   22,   19,   16,   12,    9,    6,    3,
       0,   -3,   -6,   -9,  -12,  -16,  -19,  -22,  -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
     -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,  -71,  -73,  -76,  -78,  -81,  -83,  -85,  -88,
     -90,  -92,  -94,  -96,  -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
    -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
    -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
    -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100,  -98,  -96,  -94,  -92,
     -90,  -88,  -85,  -83,  -81,  -78,  -76,  -73,  -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
     -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,  -25,  -22,  -19,  -16,  -12,   -9,   -6,   -3,
};

static const Note *melNotes;
static uint8_t melLen;
static uint8_t melIdx;
static uint32_t melNoteLeft; // samples left in current note/rest
static uint32_t melGapLeft;  // samples left in inter-note gap
static uint32_t melPhase;    // Q32 phase accumulator
static uint32_t melPhaseInc;
static uint32_t melEnvQ16;    // Q8.16 envelope, 256<<16 = unity
static uint32_t melEnvStep;   // per-sample decrement (linear to 60%)
static bool melNoteIsRest;

static bool mel_load_note() { // false = melody finished (no loop)
  if (melIdx >= melLen) {
    if (!loopFlag)
      return false;
    melIdx = 0;
  }
  const Note &n = melNotes[melIdx++];
  melNoteLeft = (uint32_t)n.ms * MELODY_RATE / 1000ul;
  melGapLeft = GAP_SAMPLES;
  melNoteIsRest = (n.freqHz == 0);
  if (!melNoteIsRest) {
    melPhase = 0;
    melPhaseInc = (uint32_t)(((uint64_t)n.freqHz << 32) / MELODY_RATE);
    melEnvQ16 = 256ul << 16;
    melEnvStep = melNoteLeft ? (102ul << 16) / melNoteLeft : 0; // -> 60%
  }
  return true;
}

static void melody_fill() {
  uint32_t freeSlots = ring_free();
  if (freeSlots < 256)
    return;
  while (freeSlots--) {
    if (melNoteLeft == 0 && melGapLeft == 0) {
      if (!mel_load_note()) {
        srcState = SRC_DRAIN;
        return;
      }
    }
    if (melNoteLeft) {
      melNoteLeft--;
      if (melNoteIsRest) {
        ring_push(2048);
      } else {
        melPhase += melPhaseInc;
        int32_t s = SINE_LUT[melPhase >> 24];
        int32_t env = (int32_t)(melEnvQ16 >> 16);
        if (melEnvQ16 > melEnvStep)
          melEnvQ16 -= melEnvStep;
        // full scale: 127 * 256 * 256 >> 12 = 2032
        ring_push((uint16_t)(2048 + ((s * env * (int32_t)volFactor) >> 12)));
      }
    } else {
      melGapLeft--;
      ring_push(2048);
    }
  }
}

// ------------------------------------------------------------------ API ---
void audio_begin() {
  // Adafruit core powers up DAC0 on PA02 and leaves it enabled; after this
  // we own DAC->DATA[0] directly from the TC2 ISR.
  analogWriteResolution(12);
  analogWrite(PIN_AUDIO_DAC, 2048);
  dacReady = true;
  tc2_init();
}

bool audio_play_wav(const char *filename, bool loop) {
  if (!dacReady || !storage_mounted() || storage_busy())
    return false;
  FatVolume *vol = storage_volume();
  if (!vol)
    return false;
  File32 f = vol->open(filename, O_RDONLY);
  if (!f)
    return false;
  uint32_t rate = wav_parse(f);
  if (rate == 0) {
    f.close();
    return false;
  }
  audio_stop();
  wavFile = f;
  wavBytesLeft = wavDataSize;
  loopFlag = loop;
  srcState = SRC_WAV;
  wav_fill(); // prime before the timer starts eating samples
  tc2_start(rate);
  return true;
}

void audio_play_melody(uint8_t id, bool loop) {
  if (!dacReady || id >= AUDIO_MELODY_COUNT)
    return;
  audio_stop();
  melNotes = MELODIES[id];
  melLen = MELODY_LEN[id];
  melIdx = 0;
  melNoteLeft = 0;
  melGapLeft = 0;
  loopFlag = loop;
  srcState = SRC_MELODY;
  melody_fill();
  tc2_start(MELODY_RATE);
}

void audio_stop() {
  tc2_stop();
  srcState = SRC_IDLE;
  rHead = 0;
  rTail = 0;
  if (wavFile.isOpen())
    wavFile.close();
  if (dacReady)
    DAC->DATA[0].reg = 2048;
}

bool audio_playing() { return srcState != SRC_IDLE; }

void audio_task() {
  switch (srcState) {
  case SRC_WAV:
    wav_fill();
    break;
  case SRC_MELODY:
    melody_fill();
    break;
  case SRC_DRAIN:
    if (rHead == rTail)
      audio_stop();
    break;
  default:
    break;
  }
}

void audio_set_volume(uint8_t vol0to10) {
  if (vol0to10 > 10)
    vol0to10 = 10;
  volFactor = VOL_TABLE[vol0to10];
}

void buzzer_start(uint16_t freqHz) { tone(PIN_BUZZER, freqHz); }

void buzzer_stop() {
  noTone(PIN_BUZZER);
  // make sure the MOSFET gate is held low even if tone() never ran
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
}
