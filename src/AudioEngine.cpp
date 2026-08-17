// AudioEngine.cpp — DMA-paced DAC playback: TC2 (MFRQ at the sample rate)
// triggers one DMAC beat per sample out of a two-half ping-pong buffer; a
// per-half IRQ flips buffers and audio_task() refills from the main loop.
// WAV streaming from the QSPI tune volume plus builtin synthesized melodies.
#include "AudioEngine.h"
#include "TuneStorage.h"
#include "pins.h"
#include <Adafruit_ZeroDMA.h>

const char *const AUDIO_MELODY_NAMES[AUDIO_MELODY_COUNT] = {
    "Sunrise", "Classic beep", "Chime"};

// ---------------------------------------------------------------- ring ---
// Two 2048-sample halves in a looped DMA descriptor chain (A -> B -> A ...):
// ~93 ms per half at 22.05 kHz, ~43 ms at 48 kHz. BLOCKACT=INT fires the
// half-complete callback without pausing the channel; audio_task() refills
// the freed half while the other plays.
#define HALF_SAMPLES 2048u
static uint16_t ring[2][HALF_SAMPLES] __attribute__((aligned(4)));
static volatile bool halfNeedsData[2]; // half is played out, free to refill
static volatile uint32_t halvesPlayed; // bumped per finished half (IRQ)
static uint8_t fillHalf;               // next half audio_task should fill
static uint32_t drainTarget;           // halvesPlayed value that ends DRAIN

enum SrcState { SRC_IDLE, SRC_WAV, SRC_MELODY, SRC_DRAIN };
static SrcState srcState = SRC_IDLE;
static bool loopFlag;
static bool dacReady; // audio_begin ran

// DMA channel (ring -> DAC). If allocation ever fails, the driver falls
// back to the old per-sample TC2 IRQ walking the same halves.
static Adafruit_ZeroDMA audioDma;
static bool dmaOk;
static volatile uint32_t isrPos; // fallback path cursor
static volatile uint8_t isrHalf;

// Digital volume, Q8 (256 = unity). Exponential-ish steps, ~3 dB apart.
static const uint16_t VOL_TABLE[11] = {0,   16,  24,  34,  48, 68,
                                       96,  128, 165, 208, 256};
static uint16_t volFactor = 165; // default vol 8

// ------------------------------------------------------------ TC2 timer ---
// GCLK0 = 120 MHz, prescaler /8 -> 15 MHz count clock; MFRQ overflows at CC0.
// Each overflow raises the TC2 DMA request = one sample beat.
#define TC2_BASE_HZ 15000000ul

static void tc2_sync_enable() {
  while (TC2->COUNT16.SYNCBUSY.bit.ENABLE) {
  }
}

static void tc2_init(bool withIrq) {
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
  if (withIrq) { // fallback only — the DMA path needs no per-sample IRQ
    TC2->COUNT16.INTENSET.reg = TC_INTENSET_OVF;
    NVIC_SetPriority(TC2_IRQn, 1);
    NVIC_ClearPendingIRQ(TC2_IRQn);
    NVIC_EnableIRQ(TC2_IRQn);
  }
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

// A half just finished playing (IRQ context, either path). Free it for
// refill; if the half now *starting* was never refilled (main loop stalled
// past a whole half period), overwrite it with silence so stale audio does
// not loop audibly — the analog of the old ISR's midpoint hold.
static void half_played(uint8_t finished) {
  halvesPlayed++;
  halfNeedsData[finished] = true;
  uint8_t next = finished ^ 1;
  if (halfNeedsData[next]) {
    for (uint32_t i = 0; i < HALF_SAMPLES; i++)
      ring[next][i] = 2048;
  }
}

static void dma_half_cb(Adafruit_ZeroDMA *) {
  // Descriptors complete strictly alternating, A first.
  half_played((uint8_t)(halvesPlayed & 1));
}

void TC2_Handler() { // fallback path only (DMA channel unavailable)
  TC2->COUNT16.INTFLAG.reg = TC_INTFLAG_OVF;
  DAC->DATA[0].reg = ring[isrHalf][isrPos];
  if (++isrPos >= HALF_SAMPLES) {
    isrPos = 0;
    uint8_t fin = isrHalf;
    isrHalf ^= 1;
    half_played(fin);
  }
}

// --- Playback high-pass filter (WAV path only) -------------------------------
// Deep bass is the most expensive content per mA and the least audible on the
// small driver: below its resonance the impedance is at minimum (max current)
// while the cone barely radiates. A 2nd-order Butterworth high-pass ahead of
// the volume scaler trims those sustained current peaks (the nPM1300's
// 1000 mA IBATLIM is the wall — see ADR-0010/0014). Builtin melodies bypass
// this on purpose: synthesized sines at ~260 Hz+ carry no sub content.
// Set to 0.0f to disable. Coefficients depend on the sample rate, so
// hpf_setup() runs at every WAV start (rates vary 8-48 kHz).
#define AUDIO_HPF_HZ 200.0f

static float hB0, hB1, hB2, hA1, hA2; // biquad coefficients (a0-normalized)
static float hX1, hX2, hY1, hY2;      // filter state
static bool hpfOn = false;

static void hpf_setup(uint32_t rate) {
  hX1 = hX2 = hY1 = hY2 = 0.0f;
  hpfOn = (AUDIO_HPF_HZ > 0.0f) && ((float)rate > AUDIO_HPF_HZ * 4.0f);
  if (!hpfOn)
    return;
  float w0 = 2.0f * (float)PI * AUDIO_HPF_HZ / (float)rate;
  float cw = cosf(w0);
  float alpha = sinf(w0) * 0.70710678f; // = sin/(2Q), Q = 1/sqrt(2)
  float a0 = 1.0f + alpha;
  hB0 = (1.0f + cw) * 0.5f / a0;
  hB1 = -(1.0f + cw) / a0;
  hB2 = hB0;
  hA1 = -2.0f * cw / a0;
  hA2 = (1.0f - alpha) / a0;
}

static inline int32_t hpf_process(int32_t centered) {
  if (!hpfOn)
    return centered;
  float x = (float)centered;
  float y = hB0 * x + hB1 * hX1 + hB2 * hX2 - hA1 * hY1 - hA2 * hY2;
  hX2 = hX1; hX1 = x;
  hY2 = hY1; hY1 = y;
  // The filter can overshoot a full-scale edge slightly; clamp to 12-bit.
  if (y > 2047.0f) y = 2047.0f;
  if (y < -2048.0f) y = -2048.0f;
  return (int32_t)y;
}

static uint16_t apply_volume(int32_t s12) {
  int32_t c = hpf_process(s12 - 2048);
  return (uint16_t)(2048 + ((c * (int32_t)volFactor) >> 8));
}

// Source ran dry mid-half: pad with midpoint, then stop once both queued
// halves (this one + the one currently playing) have been heard.
static void enter_drain(uint16_t *p, uint32_t left) {
  while (left--)
    *p++ = 2048;
  srcState = SRC_DRAIN;
  drainTarget = halvesPlayed + 2;
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

// Fill fillHalf completely (real samples, else midpoint padding + DRAIN).
static void wav_fill_half() {
  if (!storage_mounted() || storage_busy()) {
    audio_stop(); // USB host owns the drive: abandon playback
    return;
  }
  uint16_t *p = (uint16_t *)ring[fillHalf];
  uint32_t left = HALF_SAMPLES;
  while (left) {
    if (wavBytesLeft == 0) {
      if (loopFlag && wavFile.seekSet(wavDataStart)) {
        wavBytesLeft = wavDataSize;
      } else {
        break;
      }
    }
    uint32_t want = left * wavFrameBytes;
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
    if (n <= 0)
      break;
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
      *p++ = apply_volume(s12);
      left--;
    }
  }
  if (left)
    enter_drain(p, left);
  halfNeedsData[fillHalf] = false;
  fillHalf ^= 1;
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

static void melody_fill_half() {
  uint16_t *p = (uint16_t *)ring[fillHalf];
  uint32_t left = HALF_SAMPLES;
  while (left) {
    if (melNoteLeft == 0 && melGapLeft == 0 && !mel_load_note())
      break;
    if (melNoteLeft) {
      melNoteLeft--;
      if (melNoteIsRest) {
        *p++ = 2048;
      } else {
        melPhase += melPhaseInc;
        int32_t s = SINE_LUT[melPhase >> 24];
        int32_t env = (int32_t)(melEnvQ16 >> 16);
        if (melEnvQ16 > melEnvStep)
          melEnvQ16 -= melEnvStep;
        // full scale: 127 * 256 * 256 >> 12 = 2032
        *p++ = (uint16_t)(2048 + ((s * env * (int32_t)volFactor) >> 12));
      }
    } else {
      melGapLeft--;
      *p++ = 2048;
    }
    left--;
  }
  if (left)
    enter_drain(p, left);
  halfNeedsData[fillHalf] = false;
  fillHalf ^= 1;
}

// ------------------------------------------------------------------ API ---
void audio_begin() {
  // Adafruit core powers up DAC0 on PA02 and leaves it enabled; after this
  // we own DAC->DATA[0] directly (DMA beats, or the fallback TC2 ISR).
  analogWriteResolution(12);
  analogWrite(PIN_AUDIO_DAC, 2048);
  dacReady = true;

  // One DMA beat (halfword, ring -> DAC DATA) per TC2 overflow. The two
  // half descriptors are linked in a loop; BLOCKACT=INT gives a per-half
  // callback while the channel keeps running.
  audioDma.setTrigger(TC2_DMAC_ID_OVF);
  audioDma.setAction(DMA_TRIGGER_ACTON_BEAT);
  if (audioDma.allocate() == DMA_STATUS_OK) {
    audioDma.loop(true);
    DmacDescriptor *dA = audioDma.addDescriptor(
        ring[0], (void *)&DAC->DATA[0].reg, HALF_SAMPLES, DMA_BEAT_SIZE_HWORD,
        true /*src increments*/, false /*fixed dst*/);
    DmacDescriptor *dB = audioDma.addDescriptor(
        ring[1], (void *)&DAC->DATA[0].reg, HALF_SAMPLES, DMA_BEAT_SIZE_HWORD,
        true, false);
    if (dA && dB) {
      dA->BTCTRL.bit.BLOCKACT = DMA_BLOCK_ACTION_INT;
      dB->BTCTRL.bit.BLOCKACT = DMA_BLOCK_ACTION_INT;
      audioDma.setCallback(dma_half_cb);
      dmaOk = true;
    }
  }
  tc2_init(!dmaOk); // no DMA channel -> keep the per-sample IRQ path
}

// Reset the ping-pong state and pre-silence both halves (covers sources
// shorter than one half and the tail after DRAIN padding).
static void prime_reset() {
  fillHalf = 0;
  halvesPlayed = 0;
  isrHalf = 0;
  isrPos = 0;
  for (uint32_t i = 0; i < HALF_SAMPLES; i++) {
    ring[0][i] = 2048;
    ring[1][i] = 2048;
  }
  halfNeedsData[0] = halfNeedsData[1] = true;
}

static bool start_paced(uint32_t rate) {
  if (dmaOk && audioDma.startJob() != DMA_STATUS_OK)
    return false;
  tc2_start(rate);
  return true;
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
  hpf_setup(rate); // coefficients + state BEFORE priming filters samples
  prime_reset();
  wav_fill_half(); // prime before the pacer starts eating samples
  if (srcState == SRC_WAV)
    wav_fill_half();
  if (srcState == SRC_IDLE) // storage vanished mid-prime
    return false;
  if (!start_paced(rate)) {
    audio_stop();
    return false;
  }
  return true;
}

// One synthesized tone through the normal melody machinery (sine + envelope).
// Used by the low-battery farewell chirp; freq/duration in a RAM note the
// melody source plays once. Caller keeps audio_task() pumped until done.
void audio_play_beep(uint16_t freqHz, uint16_t ms) {
  static Note beep[1];
  if (!dacReady)
    return;
  audio_stop();
  beep[0].freqHz = freqHz;
  beep[0].ms = ms;
  melNotes = beep;
  melLen = 1;
  melIdx = 0;
  melNoteLeft = 0;
  melGapLeft = 0;
  loopFlag = false;
  srcState = SRC_MELODY;
  prime_reset();
  melody_fill_half();
  if (srcState == SRC_MELODY)
    melody_fill_half();
  if (!start_paced(MELODY_RATE))
    audio_stop();
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
  prime_reset();
  melody_fill_half();
  if (srcState == SRC_MELODY)
    melody_fill_half();
  if (!start_paced(MELODY_RATE))
    audio_stop();
}

void audio_stop() {
  tc2_stop();
  if (dmaOk)
    audioDma.abort();
  srcState = SRC_IDLE;
  halfNeedsData[0] = halfNeedsData[1] = false;
  if (wavFile.isOpen())
    wavFile.close();
  if (dacReady)
    DAC->DATA[0].reg = 2048;
}

bool audio_playing() { return srcState != SRC_IDLE; }

void audio_task() {
  switch (srcState) {
  case SRC_WAV:
    while (srcState == SRC_WAV && halfNeedsData[fillHalf])
      wav_fill_half();
    break;
  case SRC_MELODY:
    while (srcState == SRC_MELODY && halfNeedsData[fillHalf])
      melody_fill_half();
    break;
  case SRC_DRAIN:
    if (halvesPlayed >= drainTarget)
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
