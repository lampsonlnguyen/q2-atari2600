// SPDX-License-Identifier: GPL-2.0-or-later
// q2-atari: Q2 frontend for the StellaDS Atari 2600 core.
//
// Two modes:
//   q2-atari                  ROM picker, then one game at a time.
//   q2-atari --smoke <rom> <outprefix> [frames]
//                             Headless: run frames under QEMU, dump the
//                             finished frame(s) as PGM for the CI smoke
//                             test. No framebuffer or audio is touched.
//
// Core interface (Stella 2.8.4-era, as shipped in StellaDS):
//   Console(image, size, filename) + update() once per frame.
//   Input goes through the global myStellaEvent (Event::set).
//   Video is the TIA's palette-indexed framebuffer (160 wide) plus
//   palette(); the core also has a direct 16-bit DS framebuffer pointer
//   (myDSFramePointer) that the frontend points at a scratch buffer so
//   the core never writes through a null pointer. --smoke dumps both
//   paths so a CI run shows which one the core actually fills.
//   Audio is TIASound's sample buffer (tia_buf / tia_buf_idx) at
//   mySoundFreq Hz.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"
#include "q2shim.h"

#include "bspf.hxx"
#include "Console.hxx"
#include "Event.hxx"
#include "TIA.hxx"
#include "TIASound.hxx"
#include "Cart.hxx"

// Defined by the Q2 shim (q2shim.cpp); the core sets it when a
// Console is constructed.
extern uInt16 mySoundFreq;

#define A26_WIDTH 160
#define A26_MAX_HEIGHT 262

// The core's direct DS framebuffer lives in the Q2 shim
// (q2_ds_framebuffer, see q2shim.h): the TIA writes every scanline
// there through BG_GFX, and the frontend reads it back only if the
// indexed framebuffer turns out empty (see pick_frame). It is set by
// the TIA itself on reset, so the frontend only needs it to exist.

// ---------------------------------------------------------------------------
// Tiny 5x7 font (uppercase, digits, a little punctuation) for the picker.
// Each glyph is 7 bytes, one per row, bit 4 = leftmost column.
// ---------------------------------------------------------------------------
static const uint8_t font5x7[][7] = {
    // ' ' (32)
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    // '(' (40)
    { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 },
    // ')' (41)
    { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 },
    // '-' (45)
    { 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00 },
    // '.' (46)
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C },
    // '/' (47)
    { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 },
    // '0' (48)
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
    // '1'
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    // '2'
    { 0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F },
    // '3'
    { 0x0E, 0x11, 0x01, 0x0E, 0x01, 0x11, 0x0E },
    // '4'
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
    // '5'
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    // '6'
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
    // '7'
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    // '8'
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
    // '9'
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
    // ':' (58)
    { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 },
    // 'A' (65)
    { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    // 'B'
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
    // 'C'
    { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
    // 'D'
    { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },
    // 'E'
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
    // 'F'
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
    // 'G'
    { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F },
    // 'H'
    { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    // 'I'
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
    // 'J'
    { 0x01, 0x01, 0x01, 0x01, 0x01, 0x11, 0x0E },
    // 'K'
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
    // 'L'
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
    // 'M'
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },
    // 'N'
    { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
    // 'O'
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    // 'P'
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
    // 'Q'
    { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },
    // 'R'
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
    // 'S'
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },
    // 'T'
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
    // 'U'
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    // 'V'
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
    // 'W'
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 },
    // 'X'
    { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
    // 'Y'
    { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },
    // 'Z'
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
    // '_' (95)
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F },
};

// Index into font5x7 for a character; -1 when the character is not in
// the table (rendered as a space). Lowercase is uppercased.
static int glyph_index(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c == ' ') return 0;
    if (c == '(') return 1;
    if (c == ')') return 2;
    if (c == '-') return 3;
    if (c == '.') return 4;
    if (c == '/') return 5;
    if (c >= '0' && c <= '9') return 6 + (c - '0');
    if (c == ':') return 16;
    if (c >= 'A' && c <= 'Z') return 17 + (c - 'A');
    if (c == '_') return 43;
    return -1;
}

static void draw_text(uint32_t *cv, int x, int y, const char *s, uint32_t color) {
    for (; *s; ++s, x += 6) {
        int g = glyph_index(*s);
        if (g < 0) continue;
        for (int row = 0; row < 7; ++row) {
            uint8_t bits = font5x7[g][row];
            for (int col = 0; col < 5; ++col) {
                if (bits & (0x10 >> col)) {
                    int px = x + col, py = y + row;
                    if (px >= 0 && px < Q2_WIDTH && py >= 0 && py < Q2_HEIGHT)
                        cv[py * Q2_WIDTH + px] = color;
                }
            }
        }
    }
}

static void fill_rect(uint32_t *cv, int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > Q2_WIDTH) w = Q2_WIDTH - x;
    if (y + h > Q2_HEIGHT) h = Q2_HEIGHT - y;
    for (int row = 0; row < h; ++row) {
        uint32_t *dst = cv + (y + row) * Q2_WIDTH + x;
        for (int i = 0; i < w; ++i) dst[i] = color;
    }
}

// ---------------------------------------------------------------------------
// Explicit .init_array runner.
//
// The Q2 firmware's ld.so (glibc 2.28) does not run the main
// executable's DT_INIT_ARRAY — verified under qemu-mipsel with both a
// C __attribute__((constructor)) test and a C++ global-constructor
// test: the constructor never fires, while a shared library's
// constructor in the same process does. (A newer ld.so, e.g. Ubuntu's
// 2.39, runs it, which is why the host build works.) Without this,
// the core's globals (theTIA, theM6532, myStellaEvent, ...) are never
// constructed: TIA::reset() then dereferences the null
// myCurrentFrameBuffer and segfaults in the Console constructor.
// Running the array here, at the start of main(), fixes it on any
// loader. The guard keeps a loader that does run the array from
// double-constructing: TIA's constructor sets myCurrentFrameBuffer[0],
// so a non-null pointer means the array already ran.
// ---------------------------------------------------------------------------
extern "C" {
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
}

static void run_init_array_once(void) {
    if (myCurrentFrameBuffer[0] != nullptr) return;  // loader ran it
    for (void (**p)(void) = __init_array_start; p < __init_array_end; ++p)
        if (*p) (*p)();
    fprintf(stderr, "q2-atari: ran .init_array manually (ld.so skipped it)\n");
}

// ---------------------------------------------------------------------------
// ROM loading and discovery
// ---------------------------------------------------------------------------
static bool has_rom_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".a26") == 0 || strcasecmp(dot, ".bin") == 0;
}

static uint8_t *read_rom(const char *path, uint32_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 512 * 1024) { fclose(f); return NULL; }
    uint8_t *rom = (uint8_t *)malloc((size_t)size);
    if (!rom) { fclose(f); return NULL; }
    if (fread(rom, 1, (size_t)size, f) != (size_t)size) {
        free(rom);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (uint32_t)size;
    return rom;
}

// First directory that exists and holds ROMs, in preference order.
static bool find_rom_dir(char *out, size_t out_size) {
    const char *env = getenv("Q2_ATARI_ROMS");
    const char *candidates[] = {
        env, "/mnt/mmc/atari2600/ROMs", "./atari2600/ROMs", ".",
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; ++i) {
        const char *dir = candidates[i];
        if (!dir || !*dir) continue;
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *ent;
        bool found = false;
        while ((ent = readdir(d)) != NULL) {
            if (has_rom_ext(ent->d_name)) { found = true; break; }
        }
        closedir(d);
        if (found) {
            snprintf(out, out_size, "%s", dir);
            return true;
        }
    }
    return false;
}

static int cmp_str(const void *a, const void *b) {
    return strcasecmp((const char *)a, (const char *)b);
}

#define MAX_ROMS 500
static char rom_names[MAX_ROMS][128];
static int rom_count;

static int scan_roms(const char *dir) {
    rom_count = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && rom_count < MAX_ROMS) {
        if (!has_rom_ext(ent->d_name)) continue;
        snprintf(rom_names[rom_count], sizeof rom_names[rom_count], "%s", ent->d_name);
        ++rom_count;
    }
    closedir(d);
    qsort(rom_names, (size_t)rom_count, sizeof rom_names[0], cmp_str);
    return rom_count;
}

// ---------------------------------------------------------------------------
// Video: indexed framebuffer -> canvas, scaled to fit.
// The core keeps two indexed buffers; the one it just finished is the
// one whose content changed since last frame. The direct DS pointer
// buffer is the fallback when both indexed buffers are empty.
// ---------------------------------------------------------------------------
static uint32_t last_sum[2] = { 0, 0 };

static uint32_t buffer_sum(const uint8_t *buf, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; i += 16) s = s * 31 + buf[i];
    return s;
}

// Returns 0/1 for the indexed buffer to show, or 2 for the DS pointer.
static int pick_frame(int height) {
    size_t n = (size_t)A26_WIDTH * (size_t)height;
    uint32_t sum[2];
    bool empty[2];
    for (int b = 0; b < 2; ++b) {
        const uint8_t *buf = myCurrentFrameBuffer[b];
        sum[b] = buf ? buffer_sum(buf, n) : 0;
        empty[b] = !buf || sum[b] == 0;
    }
    // Prefer a buffer whose content changed this frame.
    for (int b = 0; b < 2; ++b) {
        if (!empty[b] && sum[b] != last_sum[b]) {
            last_sum[0] = sum[0];
            last_sum[1] = sum[1];
            return b;
        }
    }
    last_sum[0] = sum[0];
    last_sum[1] = sum[1];
    for (int b = 0; b < 2; ++b)
        if (!empty[b]) return b;
    return 2;  // indexed buffers empty; fall back to the DS pointer
}

static void blit_frame(uint32_t *cv) {
    int height = (int)theTIA.height();
    if (height < 1) height = 192;
    if (height > A26_MAX_HEIGHT) height = A26_MAX_HEIGHT;
    const uInt32 *palette = theTIA.palette();

    fill_rect(cv, 0, 0, Q2_WIDTH, Q2_HEIGHT, 0xFF000000u);
    int which = pick_frame(height);

    // Fixed-point scale to fit, letterboxed.
    int scale_num, scale_den;
    if (Q2_WIDTH * height <= Q2_HEIGHT * A26_WIDTH) {
        scale_num = Q2_WIDTH;
        scale_den = A26_WIDTH;
    } else {
        scale_num = Q2_HEIGHT;
        scale_den = height;
    }
    int dw = A26_WIDTH * scale_num / scale_den;
    int dh = height * scale_num / scale_den;
    int ox = (Q2_WIDTH - dw) / 2;
    int oy = (Q2_HEIGHT - dh) / 2;

    for (int y = 0; y < dh; ++y) {
        int sy = y * height / dh;
        uint32_t *dst = cv + (oy + y) * Q2_WIDTH + ox;
        if (which == 2) {
            const uInt16 *src = (const uInt16 *)myDSFramePointer + sy * A26_WIDTH;
            for (int x = 0; x < dw; ++x) {
                int sx = x * A26_WIDTH / dw;
                uint16_t p = src[sx];
                uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
                dst[x] = 0xFF000000u | ((r << 3 | r >> 2) << 16) |
                         ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
            }
        } else {
            const uint8_t *src = myCurrentFrameBuffer[which] + sy * A26_WIDTH;
            for (int x = 0; x < dw; ++x) {
                int sx = x * A26_WIDTH / dw;
                uint32_t c = palette ? palette[src[sx]] : 0;
                dst[x] = 0xFF000000u | (c & 0x00FFFFFFu);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Audio: drive the TIA sound generator and drain its samples into
// ALSA each frame. The DS drove Tia_process() with a TIMER2 interrupt
// at the sample rate; the Q2 frame loop drives it here instead — one
// call makes exactly one sample, so a frame needs rate/fps of them.
// Non-WAVE samples land in *aptr/*bptr (shim slots, read after each
// call); WAVE samples land in the tia_buf ring, drained below.
//
// DC fix (v0.2.0): TIASound's raw samples are UNIPOLAR. The range is
// NOT 0..3840: StellaDS's TIA poke pre-scales AUDV by << 3
// (TIA.cpp:2297: AUDV[0] = (value & 0x0f) << 3), so one channel at
// volume 15 gives Outvol 120, raw 120*128 = 15360, and both channels
// together give 0..30720 (measured: volume 8 -> raw 0/8192). Silence
// is raw 0. The old conversion `raw ^ 0x8000` turned silence into a
// constant -32768 (full negative rail) with only 3840 counts of
// variation on top — inaudible, and a huge DC offset into ALSA.
// The correct conversion centres the signal: a DC tracker (alpha
// 1/256, ~25 ms at 10 kHz) follows the signal mean, the deviation
// from it is amplified x2 (max deviation 15360 * 2 = 30720, inside
// s16) and clamped. Silence stays exactly 0. (The first version of
// this fix used x8, which is right for a 0..3840 range but clips at
// 32767 for the real 0..30720 range — measured on the fire beep.)
static int32_t audio_dc = 0;
static unsigned long audio_stat_count = 0;
static long audio_stat_sum = 0;
static int audio_stat_min = 0, audio_stat_max = 0;

static int16_t audio_convert(uint16_t raw) {
    audio_dc += ((int32_t)raw - audio_dc) >> 8;
    int32_t v = ((int32_t)raw - audio_dc) * 2;
    if (v > 32767) v = 32767;
    else if (v < -32768) v = -32768;
    int16_t s = (int16_t)v;
    if (audio_stat_count == 0) { audio_stat_min = audio_stat_max = s; }
    else {
        if (s < audio_stat_min) audio_stat_min = s;
        if (s > audio_stat_max) audio_stat_max = s;
    }
    audio_stat_sum += s;
    ++audio_stat_count;
    return s;
}

// Log the audio stats once (called when a game exits): proves the
// samples are centred (mean near 0) and not silent/clipped.
static void audio_log_stats(void) {
    if (!audio_stat_count) return;
    fprintf(stderr,
            "q2-atari: audio samples=%lu min=%d max=%d mean=%ld\n",
            audio_stat_count, audio_stat_min, audio_stat_max,
            audio_stat_sum / (long)audio_stat_count);
    audio_stat_count = 0;
    audio_stat_sum = 0;
}

static unsigned gen_audio(int16_t *out, unsigned frames) {
    unsigned n = 0;
    if (myCartInfo.soundQuality == SOUND_WAVE) {
        for (unsigned i = 0; i < frames; ++i) Tia_process();
        // Drain the tia_buf ring, advancing the read position so the
        // producer never sees a full buffer (it spins waiting for the
        // drain when full). The ring holds SOUND_SIZE entries.
        unsigned avail =
            (unsigned)((tia_buf_idx - tia_out_idx) & (SOUND_SIZE - 1));
        if (avail > frames) avail = frames;
        for (unsigned i = 0; i < avail; ++i) {
            if (getenv("Q2_ATARI_DEBUG_AUDIO") && i < 40)
                fprintf(stderr, "q2-atari: raw[%u]=%u dc=%ld\n", i,
                        (unsigned)tia_buf[tia_out_idx], (long)audio_dc);
            int16_t s = audio_convert(tia_buf[tia_out_idx]);
            tia_out_idx = (uInt16)((tia_out_idx + 1) & (SOUND_SIZE - 1));
            out[2 * n] = s;
            out[2 * n + 1] = s;
            ++n;
        }
    } else {
        for (unsigned i = 0; i < frames; ++i) {
            Tia_process();
            if (getenv("Q2_ATARI_DEBUG_AUDIO") && i < 40)
                fprintf(stderr, "q2-atari: raw[%u]=%u dc=%ld\n", i,
                        (unsigned)*aptr, (long)audio_dc);
            int16_t s = audio_convert(*aptr);
            out[2 * n] = s;
            out[2 * n + 1] = s;
            ++n;
        }
    }
    return n;
}

static void pump_audio(void) {
    if (!platform_audio_available()) return;
    unsigned rate = mySoundFreq ? mySoundFreq : 20933;
    unsigned fps = myCartInfo.tv_type == PAL ? 50 : 60;
    static unsigned audio_acc = 0;
    audio_acc += rate;
    unsigned want = audio_acc / fps;
    audio_acc %= fps;
    if (want == 0) return;
    if (want > 4096) want = 4096;
    static int16_t stereo[4096 * 2];
    unsigned got = gen_audio(stereo, want);
    if (got) platform_audio_write(stereo, got);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
struct Held {
    bool up, down, left, right, fire, play_fire;
};

static void clear_events(void) {
    myStellaEvent.set(Event::JoystickZeroUp, 0);
    myStellaEvent.set(Event::JoystickZeroDown, 0);
    myStellaEvent.set(Event::JoystickZeroLeft, 0);
    myStellaEvent.set(Event::JoystickZeroRight, 0);
    myStellaEvent.set(Event::JoystickZeroFire, 0);
    myStellaEvent.set(Event::ConsoleSelect, 0);
    myStellaEvent.set(Event::ConsoleReset, 0);
}

static void apply_held(const Held *h) {
    myStellaEvent.set(Event::JoystickZeroUp, h->up ? 1 : 0);
    myStellaEvent.set(Event::JoystickZeroDown, h->down ? 1 : 0);
    myStellaEvent.set(Event::JoystickZeroLeft, h->left ? 1 : 0);
    myStellaEvent.set(Event::JoystickZeroRight, h->right ? 1 : 0);
    // Fire is Centre held OR Play held (see drain_events).
    myStellaEvent.set(Event::JoystickZeroFire,
                      (h->fire || h->play_fire) ? 1 : 0);
}

// One-frame pulse for a console switch: set, run a frame, clear.
static void pulse_switch(Console *console, Event::Type type) {
    myStellaEvent.set(type, 1);
    console->update();
    myStellaEvent.set(type, 0);
}

// Firmware datagram key codes (AWTK, q2-pod patch/addon.c): the
// firmware sends "k d CODE" / "k u CODE" on the input socket. Return
// 170, Centre 218, Play 171, Prev/Next side buttons 222/223 (172/173
// are the same buttons on older firmware, per q2-pico8). On firmware
// too old to forward keys, the game grabs md-gpio-keys instead and
// gets "g" events with the Linux codes (q2-boy's decode_key,
// main.c:2939): Return 103, Centre 116, Play 108, Previous 165,
// Next 163. A "g" event is translated to the matching firmware code
// below, so the rest of the input handling is identical; socket "k"
// events are ignored while GPIO is grabbed, like q2-boy.
#define FW_KEY_RETURN 170
#define FW_KEY_PLAY 171
#define FW_KEY_PREV 222
#define FW_KEY_PREV_OLD 172
#define FW_KEY_NEXT 223
#define FW_KEY_NEXT_OLD 173
#define FW_KEY_CENTRE 218
#define GPIO_KEY_RETURN 103
#define GPIO_KEY_PLAY 108
#define GPIO_KEY_PREV 165
#define GPIO_KEY_NEXT 163
#define GPIO_KEY_CENTRE 116

// One wheel tick is 21 units of the firmware's raw travel (200 per
// turn, rising anticlockwise) — q2-boy's read_wheel (main.c:2917)
// uses 21 (38 in its cover-flow browser; Atari has no cover flow).
// Each event's delta is clamped to +/-84 and the accumulator resets
// after 300 ms idle, exactly like q2-boy. (v0.2.0 used 28 with no
// clamp and no expiry, citing q2-utilities' 50% = 28 conversion.)
#define WHEEL_UNITS_PER_TICK 21
#define WHEEL_MAX_DELTA 84
#define WHEEL_IDLE_RESET_NS 300000000ull

// q2-boy wheel accumulator (read_wheel, main.c:2917-2933). Feed one
// 'w' datagram's travel; returns whole ticks: >0 anticlockwise =
// up, <0 clockwise = down. Shared by the game and the picker.
struct WheelAcc { int acc = 0; uint64_t last_ns = 0; };
static int wheel_feed(WheelAcc *w, int delta, uint64_t now_ns) {
    if (w->last_ns == 0 || now_ns - w->last_ns > WHEEL_IDLE_RESET_NS)
        w->acc = 0;
    w->last_ns = now_ns;
    if (delta > WHEEL_MAX_DELTA) delta = WHEEL_MAX_DELTA;
    if (delta < -WHEEL_MAX_DELTA) delta = -WHEEL_MAX_DELTA;
    w->acc += delta;
    int ticks = w->acc / WHEEL_UNITS_PER_TICK;
    w->acc -= ticks * WHEEL_UNITS_PER_TICK;
    return ticks;
}

// When Play was pressed down (monotonic ns); the up branch uses it
// to tell a tap (<300 ms, = Select) from a hold (= fire).
static uint64_t play_down_ns_ = 0;

// Drain platform events. Returns: 0 keep playing, 1 exit to picker,
// 2 quit the app. Pulses (select/reset/etc.) are applied immediately.
// Held directions/fire are written into *held. One-frame pulses
// (wheel ticks) are reported in *pulse as a bitmask: 1 up, 2 down,
// 4 left, 8 right, 16 fire. The caller applies them around
// console->update().
static int drain_events(Console *console, Held *held, unsigned *pulse) {
    Q2Event e;
    while (platform_event(&e)) {
        if (e.kind == 'w') {
            // Raw travel arrives once per firmware UI loop pass and
            // can span several ticks; accumulate and emit one pulse
            // per tick (at most one per direction per frame — the
            // remainder stays queued for the next frame).
            // DIRECTION (fixed v0.2.0): firmware travel rises
            // anticlockwise (q2-pod: "the wheel's x falls
            // clockwise"), and the Q2 list convention — followed by
            // q2-utilities (focus -= step) and q2emu (wheel(+n) is
            // down/next) — is clockwise = down, anticlockwise = up.
            // So positive travel = UP, negative travel = DOWN. The
            // v0.1.0 code had this backwards in both the game and
            // the picker. Tick size, clamp and idle expiry follow
            // q2-boy's read_wheel (see wheel_feed above).
            static WheelAcc wheel;
            int ticks = wheel_feed(&wheel, e.x, clock_ns());
            if (ticks > 0) *pulse |= 1u;  // up
            else if (ticks < 0) *pulse |= 2u;  // down
        } else if (e.kind == 'r') {
            // q2-boy's read_event: an EV_SYN/SYN_DROPPED clears
            // every held button (a release lost in the input queue
            // must not stick a direction or fire on).
            held->up = held->down = held->left = held->right = false;
            held->fire = held->play_fire = false;
            play_down_ns_ = 0;
        } else if (e.kind == 'g' || e.kind == 'k') {
            // Normalize a raw GPIO key to its firmware code
            // (q2-boy's decode_key, main.c:2939). While GPIO is
            // grabbed, socket keys are ignored, like q2-boy.
            int key_code;
            if (e.kind == 'g') {
                switch (e.x) {
                case GPIO_KEY_RETURN: key_code = FW_KEY_RETURN; break;
                case GPIO_KEY_PLAY: key_code = FW_KEY_PLAY; break;
                case GPIO_KEY_PREV: key_code = FW_KEY_PREV; break;
                case GPIO_KEY_NEXT: key_code = FW_KEY_NEXT; break;
                case GPIO_KEY_CENTRE: key_code = FW_KEY_CENTRE; break;
                default: continue;
                }
            } else {
                if (platform_raw_buttons()) continue;
                key_code = e.x;
            }
            bool down = e.action == 'd';
            fprintf(stderr, "q2-atari: firmware key %d %s\n", key_code,
                    down ? "down" : "up");
            switch (key_code) {
            case FW_KEY_CENTRE:
                held->fire = down;
                break;
            case FW_KEY_PREV: case FW_KEY_PREV_OLD:
                held->left = down;
                break;
            case FW_KEY_NEXT: case FW_KEY_NEXT_OLD:
                held->right = down;
                break;
            case FW_KEY_PLAY:
                // Play is fire while held, and a short tap (<300 ms)
                // is Console Select — so one button does both without
                // Select firing a shot: on release, fire is cleared
                // before the Select pulse runs its frame. Centre
                // remains fire too (the v0.1.0 mapping, and the Q2
                // convention that Centre is the primary action).
                if (down) {
                    held->play_fire = true;
                    play_down_ns_ = clock_ns();
                } else {
                    held->play_fire = false;
                    myStellaEvent.set(Event::JoystickZeroFire,
                                      held->fire ? 1 : 0);
                    if (clock_ns() - play_down_ns_ < 300000000ull)
                        pulse_switch(console, Event::ConsoleSelect);
                }
                break;
            case FW_KEY_RETURN:
                // Exit on the press, before the firmware TERMs the
                // game for holding Return; its release is eaten by
                // the firmware, so the picker never sees it.
                if (down) return 1;
                break;
            default:
                break;
            }
        } else if (e.kind == 't' && e.action == 'd') {
            // Touch down only: a move or lift across the strip must
            // not re-trigger the switch. Bottom touch strip: four
            // console-switch zones.
            if (e.y >= 280) {
                int zone = e.x / (Q2_WIDTH / 4);
                if (zone == 0) pulse_switch(console, Event::ConsoleSelect);
                else if (zone == 1) pulse_switch(console, Event::ConsoleReset);
                else if (zone == 2) {
                    static bool color = true;
                    color = !color;
                    myStellaEvent.set(Event::ConsoleColor, color ? 1 : 0);
                    myStellaEvent.set(Event::ConsoleBlackWhite, color ? 0 : 1);
                } else {
                    static bool diff_a = true;
                    diff_a = !diff_a;
                    myStellaEvent.set(Event::ConsoleLeftDifficultyA, diff_a ? 1 : 0);
                    myStellaEvent.set(Event::ConsoleLeftDifficultyB, diff_a ? 0 : 1);
                }
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// ROM picker
// ---------------------------------------------------------------------------
static void draw_picker(uint32_t *cv, const char *dir, int selected, int top) {
    fill_rect(cv, 0, 0, Q2_WIDTH, Q2_HEIGHT, 0xFF000000u);
    draw_text(cv, 12, 10, "ATARI 2600", 0xFFFFFFFFu);
    char dir_line[160];
    snprintf(dir_line, sizeof dir_line, "ROMS: %s", dir);
    draw_text(cv, 12, 26, dir_line, 0xFF888888u);
    if (rom_count == 0) {
        draw_text(cv, 12, 60, "NO ROMS FOUND", 0xFFFF5555u);
        draw_text(cv, 12, 80, "PUT .A26 / .BIN FILES IN", 0xFFAAAAAAu);
        draw_text(cv, 12, 96, "ATARI2600/ROMS ON THE SD CARD", 0xFFAAAAAAu);
    } else {
        int rows = (Q2_HEIGHT - 60) / 14;
        for (int i = 0; i < rows && top + i < rom_count; ++i) {
            int idx = top + i;
            int y = 52 + i * 14;
            if (idx == selected)
                fill_rect(cv, 8, y - 2, Q2_WIDTH - 16, 12, 0xFF222288u);
            char name[64];
            snprintf(name, sizeof name, "%s", rom_names[idx]);
            // Strip the extension for display.
            char *dot = strrchr(name, '.');
            if (dot) *dot = 0;
            draw_text(cv, 12, y, name, idx == selected ? 0xFFFFFFFFu : 0xFFBBBBBB);
        }
    }
    draw_text(cv, 12, Q2_HEIGHT - 14, "WHEEL: BROWSE  CENTRE: LAUNCH  PLAY: NEXT  RETURN: QUIT",
              0xFF888888u);
}

// Returns 0 to quit, 1 with *out_path set to the chosen ROM.
static int run_picker(char *out_path, size_t out_size) {
    char dir[256];
    if (!find_rom_dir(dir, sizeof dir)) {
        snprintf(dir, sizeof dir, "/mnt/mmc/atari2600/ROMs");
        rom_count = 0;
    } else {
        scan_roms(dir);
    }
    int selected = 0, top = 0;
    uint32_t *cv = platform_canvas();
    draw_picker(cv, dir, selected, top);
    platform_present();

    while (!q2_quitting) {
        Q2Event e;
        if (!platform_event(&e)) {
            platform_wait(UINT64_MAX);
            continue;
        }
        if (e.kind == 'w' && rom_count > 0) {
            // Wheel: q2-boy's read_wheel (21 units/tick, clamp,
            // 300 ms idle expiry — see wheel_feed). Direction:
            // positive travel is anticlockwise = up the list,
            // negative (clockwise) = down — same convention as the
            // game (see drain_events); v0.1.0 had this backwards.
            static WheelAcc wheel;
            int ticks = wheel_feed(&wheel, e.x, clock_ns());
            selected -= ticks;
            if (selected < 0) selected = 0;
            if (selected > rom_count - 1) selected = rom_count - 1;
        } else if ((e.kind == 'k' || e.kind == 'g') && e.action == 'd') {
            // Same key normalization as drain_events: a raw GPIO
            // key translates to its firmware code; socket keys are
            // ignored while GPIO is grabbed (q2-boy's decode_key).
            int key_code;
            if (e.kind == 'g') {
                switch (e.x) {
                case GPIO_KEY_RETURN: key_code = FW_KEY_RETURN; break;
                case GPIO_KEY_PLAY: key_code = FW_KEY_PLAY; break;
                case GPIO_KEY_PREV: key_code = FW_KEY_PREV; break;
                case GPIO_KEY_NEXT: key_code = FW_KEY_NEXT; break;
                case GPIO_KEY_CENTRE: key_code = FW_KEY_CENTRE; break;
                default: continue;
                }
            } else {
                if (platform_raw_buttons()) continue;
                key_code = e.x;
            }
            if (key_code == FW_KEY_CENTRE && rom_count > 0) {
                snprintf(out_path, out_size, "%s/%s", dir, rom_names[selected]);
                return 1;
            } else if (key_code == FW_KEY_PLAY && rom_count > 0) {
                // q2-boy's browser: Play = next ROM (+1); Centre
                // launches. (v0.2.0 had Play launch too.)
                if (selected < rom_count - 1) ++selected;
            } else if (key_code == FW_KEY_RETURN) {
                return 0;
            } else if ((key_code == FW_KEY_PREV || key_code == FW_KEY_PREV_OLD) &&
                       selected > 0) {
                --selected;
            } else if ((key_code == FW_KEY_NEXT || key_code == FW_KEY_NEXT_OLD) &&
                       selected < rom_count - 1) {
                ++selected;
            }
        } else if (e.kind == 't' && e.action == 'd' && rom_count > 0) {
            // Tap a row to select it; tap the selected row to launch.
            int row = (e.y - 52) / 14;
            if (row >= 0 && top + row < rom_count) {
                if (top + row == selected) {
                    snprintf(out_path, out_size, "%s/%s", dir, rom_names[selected]);
                    return 1;
                }
                selected = top + row;
            }
        }
        int rows = (Q2_HEIGHT - 60) / 14;
        if (selected < top) top = selected;
        if (selected >= top + rows) top = selected - rows + 1;
        draw_picker(cv, dir, selected, top);
        platform_present();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Game loop
// ---------------------------------------------------------------------------
static int run_game(const char *rom_path) {
    uint32_t rom_size = 0;
    uint8_t *rom = read_rom(rom_path, &rom_size);
    if (!rom) {
        fprintf(stderr, "q2-atari: cannot read ROM %s\n", rom_path);
        return 0;
    }
    q2shim_defaults_for_rom(rom_path);
    // The TIA resets myDSFramePointer to BG_GFX (the shim buffer)
    // during Console construction; point it there first so it is
    // never null in between.
    myDSFramePointer = q2_ds_framebuffer;
    // The core's sound buffers live at fixed DS VRAM addresses; map
    // real RAM there before the Console constructor runs
    // Tia_sound_init(), or it segfaults writing to unmapped memory.
    if (!q2shim_map_ds_ram()) {
        fprintf(stderr, "q2-atari: cannot map DS sound RAM\n");
        free(rom);
        return 0;
    }

    Console *console = new Console(rom, rom_size, rom_path);
    fprintf(stderr, "q2-atari: loaded %s (%u bytes), cart %s\n", rom_path,
            rom_size, console->myCartridge ? console->myCartridge->name() : "?");

    // Paddle games: park the paddle mid-range so the game is at least
    // visible until paddle input is mapped.
    if (myCartInfo.controllerType == CTR_PADDLE0 ||
        myCartInfo.controllerType == CTR_PADDLE1 ||
        myCartInfo.controllerType == CTR_PADDLE2 ||
        myCartInfo.controllerType == CTR_PADDLE3) {
        myStellaEvent.set(Event::PaddleZeroResistance, 500000);
        myStellaEvent.set(Event::PaddleOneResistance, 500000);
    }
    platform_audio_set_rate(mySoundFreq ? mySoundFreq : 20933);

    uint64_t period_ns =
        myCartInfo.tv_type == PAL ? 1000000000ull / 50 : 1000000000ull / 60;
    uint64_t next = clock_ns() + period_ns;
    Held held = {};
    uint32_t *cv = platform_canvas();
    int rc = 0;  // 0 quit app, 1 back to picker

    while (!q2_quitting) {
        unsigned pulse = 0;
        int action = drain_events(console, &held, &pulse);
        if (action == 1) { rc = 1; break; }
        if (action == 2) { rc = 0; break; }
        apply_held(&held);
        // One-frame pulses on top of the held state.
        if (pulse & 1u) myStellaEvent.set(Event::JoystickZeroUp, 1);
        if (pulse & 2u) myStellaEvent.set(Event::JoystickZeroDown, 1);
        if (pulse & 4u) myStellaEvent.set(Event::JoystickZeroLeft, 1);
        if (pulse & 8u) myStellaEvent.set(Event::JoystickZeroRight, 1);
        if (pulse & 16u) myStellaEvent.set(Event::JoystickZeroFire, 1);
        console->update();
        // Restore the held state after the pulse frame.
        if (pulse & 1u) myStellaEvent.set(Event::JoystickZeroUp, held.up ? 1 : 0);
        if (pulse & 2u) myStellaEvent.set(Event::JoystickZeroDown, held.down ? 1 : 0);
        if (pulse & 4u) myStellaEvent.set(Event::JoystickZeroLeft, held.left ? 1 : 0);
        if (pulse & 8u) myStellaEvent.set(Event::JoystickZeroRight, held.right ? 1 : 0);
        if (pulse & 16u) myStellaEvent.set(Event::JoystickZeroFire,
                                           (held.fire || held.play_fire) ? 1 : 0);

        ++gAtariFrames;
        ++gTotalAtariFrames;
        pump_audio();
        blit_frame(cv);
        platform_present();

        uint64_t now = clock_ns();
        if (now > next + 3 * period_ns) next = now;  // fell behind: resync
        platform_wait(next);
        next += period_ns;
    }

    clear_events();
    audio_log_stats();
    delete console;
    free(rom);
    return rc;
}

// ---------------------------------------------------------------------------
// --smoke mode: headless frame dump for the CI QEMU test.
// Prints SMOKE lines for the job to grep, writes <prefix>-indexed0.pgm,
// <prefix>-indexed1.pgm and <prefix>-ds.pgm (160xH greyscale).
// ---------------------------------------------------------------------------
static uint8_t luminance(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
}

static void write_pgm(const char *path, const uint8_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(px, 1, (size_t)w * (size_t)h, f);
    fclose(f);
}

static int run_smoke(const char *rom_path, const char *prefix, int frames) {
    uint32_t rom_size = 0;
    uint8_t *rom = read_rom(rom_path, &rom_size);
    if (!rom) {
        printf("SMOKE_FAIL cannot read ROM %s\n", rom_path);
        return 1;
    }
    q2shim_defaults_for_rom(rom_path);
    myDSFramePointer = q2_ds_framebuffer;
    if (!q2shim_map_ds_ram()) {
        printf("SMOKE_FAIL cannot map DS sound RAM\n");
        free(rom);
        return 1;
    }
    Console *console = new Console(rom, rom_size, rom_path);
    printf("SMOKE cart=%s size=%u tv=%s\n",
           console->myCartridge ? console->myCartridge->name() : "?",
           rom_size, myCartInfo.tv_type == PAL ? "PAL" : "NTSC");
    // Scripted input for the playable-ROM test (env
    // Q2_ATARI_SMOKE_INPUT="frame:event,frame:event", events:
    // up,down,left,right,fire,fireoff,upoff,downoff,leftoff,rightoff,
    // wheelup,wheeldown (one-frame pulses), select (one-frame pulse)).
    // Lets QEMU prove joystick/fire work without booting the emulator.
    // Held events persist until their *off event; pulses last one frame.
    const char *script = getenv("Q2_ATARI_SMOKE_INPUT");
    bool s_up = false, s_down = false, s_left = false, s_right = false,
         s_fire = false;
    for (int i = 0; i < frames; ++i) {
        if (script) {
            // Re-scan the script each frame (it is short).
            bool p_up = false, p_down = false, p_select = false;
            char *copy = strdup(script);
            char *save = NULL;
            for (char *tok = strtok_r(copy, ",", &save); tok;
                 tok = strtok_r(NULL, ",", &save)) {
                int at = 0;
                char ev[32] = { 0 };
                if (sscanf(tok, "%d:%31s", &at, ev) != 2 || at != i) continue;
                if (!strcmp(ev, "up")) s_up = true;
                else if (!strcmp(ev, "down")) s_down = true;
                else if (!strcmp(ev, "left")) s_left = true;
                else if (!strcmp(ev, "right")) s_right = true;
                else if (!strcmp(ev, "fire")) s_fire = true;
                else if (!strcmp(ev, "upoff")) s_up = false;
                else if (!strcmp(ev, "downoff")) s_down = false;
                else if (!strcmp(ev, "leftoff")) s_left = false;
                else if (!strcmp(ev, "rightoff")) s_right = false;
                else if (!strcmp(ev, "fireoff")) s_fire = false;
                else if (!strcmp(ev, "wheelup")) p_up = true;
                else if (!strcmp(ev, "wheeldown")) p_down = true;
                else if (!strcmp(ev, "select")) p_select = true;
            }
            free(copy);
            myStellaEvent.set(Event::JoystickZeroUp,
                              (s_up || p_up) ? 1 : 0);
            myStellaEvent.set(Event::JoystickZeroDown,
                              (s_down || p_down) ? 1 : 0);
            myStellaEvent.set(Event::JoystickZeroLeft, s_left ? 1 : 0);
            myStellaEvent.set(Event::JoystickZeroRight, s_right ? 1 : 0);
            myStellaEvent.set(Event::JoystickZeroFire, s_fire ? 1 : 0);
            myStellaEvent.set(Event::ConsoleSelect, p_select ? 1 : 0);
        }
        console->update();
        // One-frame pulses clear after the frame they fired in.
        myStellaEvent.set(Event::JoystickZeroUp, s_up ? 1 : 0);
        myStellaEvent.set(Event::JoystickZeroDown, s_down ? 1 : 0);
        myStellaEvent.set(Event::ConsoleSelect, 0);
        ++gAtariFrames;
        ++gTotalAtariFrames;
    }

    // Audio probe: drive one frame's worth of samples to prove the
    // TIA sound path runs (the DS drove it with a TIMER2 interrupt;
    // the Q2 frame loop drives it). The test ROM never touches the
    // audio registers, so the samples are silence — the check is that
    // generation runs without crashing and returns the count. With
    // the v0.2.0 DC fix, silence must be 0 (old code: -32768).
    static int16_t probe[512 * 2];
    unsigned audio_got = gen_audio(probe, 349);
    if (getenv("Q2_ATARI_DEBUG_AUDIO")) {
        // Converted probe values (first 40), for amplitude diagnosis.
        fprintf(stderr, "q2-atari: probe first 40:");
        for (int i = 0; i < 40 && i < (int)audio_got; ++i)
            fprintf(stderr, " %d", (int)probe[2 * i]);
        fprintf(stderr, "\n");
    }
    int audio_min = 0, audio_max = 0;
    long audio_sum = 0;
    for (unsigned i = 0; i < audio_got; ++i) {
        int16_t s = probe[2 * i];
        if (i == 0) audio_min = audio_max = s;
        else {
            if (s < audio_min) audio_min = s;
            if (s > audio_max) audio_max = s;
        }
        audio_sum += s;
    }
    printf("SMOKE audio samples=%u tia_buf_idx=%u min=%d max=%d mean=%ld\n",
           audio_got, (unsigned)tia_buf_idx, audio_min, audio_max,
           audio_got ? audio_sum / (long)audio_got : 0);

    int height = (int)theTIA.height();
    if (height < 1) height = 192;
    if (height > A26_MAX_HEIGHT) height = A26_MAX_HEIGHT;
    printf("SMOKE height=%d sound_freq=%u tia_buf_idx=%u\n", height,
           (unsigned)mySoundFreq, (unsigned)tia_buf_idx);

    const uInt32 *palette = theTIA.palette();
    static uint8_t px[A26_WIDTH * A26_MAX_HEIGHT];
    uint32_t max_indexed[2] = { 0, 0 };
    for (int b = 0; b < 2; ++b) {
        const uint8_t *src = myCurrentFrameBuffer[b];
        uint32_t mx = 0;
        if (src) {
            for (int i = 0; i < A26_WIDTH * height; ++i) {
                uint8_t v = luminance(palette ? palette[src[i]] : 0);
                px[i] = v;
                if (v > mx) mx = v;
            }
        } else {
            memset(px, 0, (size_t)A26_WIDTH * height);
        }
        max_indexed[b] = mx;
        char path[512];
        snprintf(path, sizeof path, "%s-indexed%d.pgm", prefix, b);
        write_pgm(path, px, A26_WIDTH, height);
    }
    uint32_t max_ds = 0;
    for (int i = 0; i < A26_WIDTH * height; ++i) {
        uint16_t p = q2_ds_framebuffer[i];
        uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        uint8_t v = (uint8_t)(((r << 3) * 77 + (g << 2) * 150 + (b << 3) * 29) >> 8);
        px[i] = v;
        if (v > max_ds) max_ds = v;
    }
    char path[512];
    snprintf(path, sizeof path, "%s-ds.pgm", prefix);
    write_pgm(path, px, A26_WIDTH, height);

    printf("SMOKE max_indexed0=%u max_indexed1=%u max_ds=%u\n",
           max_indexed[0], max_indexed[1], max_ds);
    bool ok = max_indexed[0] > 16 || max_indexed[1] > 16 || max_ds > 16;
    printf(ok ? "SMOKE_OK frame is not blank\n"
              : "SMOKE_BLANK all framebuffer paths are blank\n");
    delete console;
    free(rom);
    return ok ? 0 : 1;
}

// q2-boy's has_cap (main.c:665): true when caps holds word as a
// whole whitespace-separated word. /tmp/q2-addon.caps lists the
// firmware's card-app features; "gameview" means it forwards keys
// to the game on the input socket (q2-boy's main.c:3588).
#define ADDON_CAPS "/tmp/q2-addon.caps"
static bool has_cap(const char *caps, const char *word) {
    size_t length = strlen(word);
    for (const char *at = caps; *at;) {
        at += strspn(at, " \t\r\n");
        size_t n = strcspn(at, " \t\r\n");
        if (n == length && !strncmp(at, word, n)) return true;
        at += n;
    }
    return false;
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    // The core's globals need their constructors before anything
    // else runs (see run_init_array_once above).
    run_init_array_once();

    if (argc >= 2 && strcmp(argv[1], "--smoke") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: %s --smoke <rom> <outprefix> [frames]\n",
                    argv[0]);
            return 2;
        }
        int frames = argc >= 5 ? atoi(argv[4]) : 70;
        return run_smoke(argv[2], argv[3], frames);
    }
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("q2-atari: Atari 2600 emulator for the Shanling Q2\n"
               "usage: %s [--smoke <rom> <outprefix> [frames]]\n",
               argv[0]);
        return 0;
    }

    // Firmware keys (q2-boy's main.c:3588): when the firmware
    // advertises "gameview" in /tmp/q2-addon.caps it forwards keys
    // on the input socket, so the game must not grab GPIO. On older
    // firmware without the cap, platform_open grabs md-gpio-keys
    // and the game reads raw 'g' events instead.
    char caps[512] = { 0 };
    FILE *caps_file = fopen(ADDON_CAPS, "r");
    if (caps_file) {
        size_t n = fread(caps, 1, sizeof caps - 1, caps_file);
        caps[n] = 0;
        fclose(caps_file);
    }
    bool firmware_keys = has_cap(caps, "gameview");
    fprintf(stderr, "q2-atari: firmware_keys=%d (caps: %s)\n",
            firmware_keys ? 1 : 0, caps);
    if (!platform_open(firmware_keys)) {
        fprintf(stderr, "q2-atari: platform_open failed\n");
        return 1;
    }
    char rom_path[512];
    int rc = 0;
    while (!q2_quitting) {
        if (!run_picker(rom_path, sizeof rom_path)) break;
        rc = run_game(rom_path);
        if (rc == 0) break;
    }
    platform_close();
    return rc;
}
