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

// Scratch RGB565 buffer for the core's direct DS framebuffer pointer.
// 160x262 u16. The core may write here; the frontend reads it back
// only if the indexed framebuffer turns out empty (see pick_frame).
static uInt16 ds_scratch[A26_WIDTH * A26_MAX_HEIGHT];

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
// Audio: drain TIASound's sample buffer into ALSA each frame.
// TIASound produces unsigned 16-bit samples centred on 32768; convert
// to signed and duplicate to stereo. (If the audio sounds wrong on
// device, this conversion is the first thing to revisit.)
// ---------------------------------------------------------------------------
static void pump_audio(void) {
    if (!platform_audio_available()) return;
    unsigned available = tia_buf_idx;
    if (available == 0 || !tia_buf) return;
    if (available > 4096) available = 4096;
    static int16_t stereo[4096 * 2];
    for (unsigned i = 0; i < available; ++i) {
        int16_t s = (int16_t)(tia_buf[i] ^ 0x8000u);
        stereo[2 * i] = s;
        stereo[2 * i + 1] = s;
    }
    tia_buf_idx = 0;
    platform_audio_write(stereo, available);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
struct Held {
    bool up, down, left, right, fire;
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
    myStellaEvent.set(Event::JoystickZeroFire, h->fire ? 1 : 0);
}

// One-frame pulse for a console switch: set, run a frame, clear.
static void pulse_switch(Console *console, Event::Type type) {
    myStellaEvent.set(type, 1);
    console->update();
    myStellaEvent.set(type, 0);
}

// Linux input key codes for the Q2's physical buttons. These are the
// standard codes; the exact mapping is confirmed from the emulator's
// demo.log (every GPIO event is logged there) on first device test.
#define KEY_ESC 1
#define KEY_ENTER 28
#define KEY_SPACE 57
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_UP 103
#define KEY_DOWN 108
#define KEY_BACKSPACE 14
#define KEY_PLAYPAUSE 164

// Drain platform events. Returns: 0 keep playing, 1 exit to picker,
// 2 quit the app. Pulses (select/reset/etc.) are applied immediately.
// Held directions/fire are written into *held. One-frame pulses
// (wheel ticks and firmware key presses, which carry no release) are
// reported in *pulse as a bitmask: 1 up, 2 down, 4 left, 8 right,
// 16 fire. The caller applies them around console->update().
static int drain_events(Console *console, Held *held, unsigned *pulse) {
    Q2Event e;
    while (platform_event(&e)) {
        if (e.kind == 'g') {
            bool down = e.action == 'd';
            fprintf(stderr, "q2-atari: gpio key code=%d %s\n", e.x,
                    down ? "down" : "up");
            switch (e.x) {
            case KEY_UP: held->up = down; break;
            case KEY_DOWN: held->down = down; break;
            case KEY_LEFT: held->left = down; break;
            case KEY_RIGHT: held->right = down; break;
            case KEY_ENTER: case KEY_SPACE: held->fire = down; break;
            case KEY_PLAYPAUSE:
                if (down) pulse_switch(console, Event::ConsoleSelect);
                break;
            case KEY_ESC: case KEY_BACKSPACE:
                if (down) return 1;
                break;
            default: break;
            }
        } else if (e.kind == 'w') {
            // Each wheel tick is a one-frame directional pulse.
            *pulse |= e.x > 0 ? 2u : 1u;
        } else if (e.kind == 'k') {
            fprintf(stderr, "q2-atari: firmware key '%c' %d %d\n",
                    e.action, e.x, e.y);
            switch (e.action) {
            case 'u': *pulse |= 1u; break;
            case 'd': *pulse |= 2u; break;
            case 'l': *pulse |= 4u; break;
            case 'r': *pulse |= 8u; break;
            case 'f': case 'a': case 'c': *pulse |= 16u; break;
            case 's': pulse_switch(console, Event::ConsoleSelect); break;
            case 'e': pulse_switch(console, Event::ConsoleReset); break;
            case 'q': return 2;
            default: break;
            }
        } else if (e.kind == 't') {
            // Bottom touch strip: four console-switch zones.
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
        } else if (e.kind == 'r') {
            return 1;  // input resync after a dropped event burst
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
    draw_text(cv, 12, Q2_HEIGHT - 14, "WHEEL: BROWSE  CENTRE: LAUNCH  RETURN: QUIT",
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
        if (e.kind == 'g' && e.action == 'd') {
            if (e.x == KEY_UP && selected > 0) --selected;
            else if (e.x == KEY_DOWN && selected < rom_count - 1) ++selected;
            else if (e.x == KEY_ENTER && rom_count > 0) {
                snprintf(out_path, out_size, "%s/%s", dir, rom_names[selected]);
                return 1;
            } else if (e.x == KEY_ESC || e.x == KEY_BACKSPACE) {
                return 0;
            }
        } else if (e.kind == 'w' && rom_count > 0) {
            selected += e.x > 0 ? 1 : -1;
            if (selected < 0) selected = 0;
            if (selected > rom_count - 1) selected = rom_count - 1;
        } else if (e.kind == 'k') {
            if (e.action == 'u' && selected > 0) --selected;
            else if (e.action == 'd' && selected < rom_count - 1) ++selected;
            else if ((e.action == 'c' || e.action == 'f') && rom_count > 0) {
                snprintf(out_path, out_size, "%s/%s", dir, rom_names[selected]);
                return 1;
            } else if (e.action == 'q') {
                return 0;
            }
        } else if (e.kind == 't' && rom_count > 0) {
            // Tap a row to select it; tap the selected row to launch.
            int row = (e.y - 52) / 14;
            if (row >= 0 && top + row < rom_count) {
                if (top + row == selected) {
                    snprintf(out_path, out_size, "%s/%s", dir, rom_names[selected]);
                    return 1;
                }
                selected = top + row;
            }
        } else if (e.kind == 'r') {
            continue;
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
    // Point the core's direct DS framebuffer at a scratch buffer so it
    // never writes through a null pointer.
    myDSFramePointer = ds_scratch;

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
        if (pulse & 16u) myStellaEvent.set(Event::JoystickZeroFire, held.fire ? 1 : 0);

        pump_audio();
        blit_frame(cv);
        platform_present();

        uint64_t now = clock_ns();
        if (now > next + 3 * period_ns) next = now;  // fell behind: resync
        platform_wait(next);
        next += period_ns;
    }

    clear_events();
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
    myDSFramePointer = ds_scratch;
    Console *console = new Console(rom, rom_size, rom_path);
    printf("SMOKE cart=%s size=%u tv=%s\n",
           console->myCartridge ? console->myCartridge->name() : "?",
           rom_size, myCartInfo.tv_type == PAL ? "PAL" : "NTSC");
    for (int i = 0; i < frames; ++i) console->update();

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
        uint16_t p = ds_scratch[i];
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

// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
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

    if (!platform_open()) {
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
