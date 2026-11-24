// SPDX-License-Identifier: GPL-2.0-or-later
// Q2 platform layer for q2-atari2600. See platform.h for the contract.
//
// Framebuffer: the panel is 320 wide x 375 tall, 32-bit XRGB. The
// logical canvas is 375x320, so a present rotates it 90 degrees into
// the back page and pans the display to it. The two pages are the two
// 375-line halves of the virtual framebuffer.
// Audio: ALSA is resolved at runtime (dlopen("libasound.so.2")) so the
// static binary carries no link-time ALSA dependency. If ALSA cannot
// be opened, the game runs silent.
// Input: a datagram socket at /tmp/q2-atari.input.sock, where the
// firmware sends wheel ('w'), key ('k') and touch ('t') events as
// short text lines — unless the firmware is too old to forward keys,
// in which case the game grabs the md-gpio-keys device itself,
// exactly like q2-boy (platform.c grab_buttons, main.c
// firmware_keys): platform_open takes the firmware_keys flag (true
// when /tmp/q2-addon.caps contains "gameview") and only grabs when
// it is false. An unconditional EVIOCGRAB would stop the firmware
// seeing the presses, so it would never send the datagrams at all
// (the v0.1.0 bug); with the cap check the grab only happens on
// firmware that never forwards keys in the first place.
#include "platform.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define INPUT_SOCKET "/tmp/q2-atari.input.sock"

static int fb = -1, keys = -1, sock = -1, back, stride, original_y;
static int stop_pipe[2] = { -1, -1 };
static int firmware_buttons;
static int raw_buttons;
static volatile sig_atomic_t signal_writer = -1;
static struct fb_var_screeninfo video;
static size_t map_size;
static uint32_t *map;
static uint32_t canvas[Q2_WIDTH * Q2_HEIGHT];

volatile sig_atomic_t q2_quitting;

uint64_t clock_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static void request_quit(int sig) {
    (void)sig;
    int saved_errno = errno;
    q2_quitting = 1;
    if (signal_writer >= 0) (void)write(signal_writer, "q", 1);
    errno = saved_errno;
}

// q2-boy's grab_buttons (platform.c:54-67), verbatim: find the
// md-gpio-keys device and grab it exclusively, so the game reads the
// physical keys itself. Only called when the firmware does not
// forward keys (no "gameview" in /tmp/q2-addon.caps).
static void grab_buttons(void) {
    for (int i = 0; i < 32; ++i) {
        char path[48], name[80] = { 0 };
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0 &&
            !strcmp(name, "md-gpio-keys") && ioctl(fd, EVIOCGRAB, 1) == 0) {
            keys = fd;
            raw_buttons = 1;
            return;
        }
        close(fd);
    }
}

int platform_raw_buttons(void) { return raw_buttons; }

// --- ALSA, resolved at runtime -------------------------------------------
// Only the handful of entry points the frontend needs. The numeric
// constants are the stable ALSA ABI values (SND_PCM_STREAM_PLAYBACK 0,
// SND_PCM_FORMAT_S16_LE 2, SND_PCM_ACCESS_RW_INTERLEAVED 3).
typedef void snd_pcm_t;
static void *alsa_handle;
static int (*alsa_open)(snd_pcm_t **, const char *, int, int);
static int (*alsa_set_params)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
static long (*alsa_writei)(snd_pcm_t *, const void *, unsigned long);
static int (*alsa_close)(snd_pcm_t *);
static int (*alsa_drop)(snd_pcm_t *);
static int (*alsa_prepare)(snd_pcm_t *);
static int (*alsa_recover)(snd_pcm_t *, int, int);

static snd_pcm_t *pcm;
static unsigned sample_rate = 20933;
#define PCM_QUEUE 2048
static int16_t audio_queue[PCM_QUEUE][2];
static unsigned audio_head, audio_count;

static void open_audio(void) {
    if (!alsa_handle) {
        alsa_handle = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!alsa_handle) return;
        // C-style casts: platform.c is also compiled with the C++
        // compiler by build.sh / CI (g++ treats .c as C++), where a
        // void* does not implicitly convert to a function pointer.
        alsa_open = (int (*)(snd_pcm_t **, const char *, int, int))dlsym(alsa_handle, "snd_pcm_open");
        alsa_set_params = (int (*)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned))dlsym(alsa_handle, "snd_pcm_set_params");
        alsa_writei = (long (*)(snd_pcm_t *, const void *, unsigned long))dlsym(alsa_handle, "snd_pcm_writei");
        alsa_close = (int (*)(snd_pcm_t *))dlsym(alsa_handle, "snd_pcm_close");
        alsa_drop = (int (*)(snd_pcm_t *))dlsym(alsa_handle, "snd_pcm_drop");
        alsa_prepare = (int (*)(snd_pcm_t *))dlsym(alsa_handle, "snd_pcm_prepare");
        alsa_recover = (int (*)(snd_pcm_t *, int, int))dlsym(alsa_handle, "snd_pcm_recover");
        if (!alsa_open || !alsa_set_params || !alsa_writei || !alsa_close ||
            !alsa_drop || !alsa_prepare || !alsa_recover) {
            dlclose(alsa_handle);
            alsa_handle = NULL;
            return;
        }
    }
    for (int i = 0; i < 20 && !q2_quitting; ++i) {
        if (alsa_open(&pcm, "plughw:0,0", 0, 0) == 0) {
            if (alsa_set_params(pcm, 2, 3, 2, sample_rate, 1, 68000) == 0)
                return;
            alsa_close(pcm);
            pcm = NULL;
        }
        usleep(100000);
    }
    fprintf(stderr, "q2-atari: ALSA open failed, running silent\n");
}

void platform_audio_set_rate(unsigned rate) {
    if (sample_rate == rate) return;
    sample_rate = rate;
    audio_head = audio_count = 0;
    if (pcm) {
        alsa_drop(pcm);
        if (alsa_set_params(pcm, 2, 3, 2, sample_rate, 1, 68000) != 0) {
            fprintf(stderr, "q2-atari: could not set audio rate %u Hz\n", rate);
            alsa_close(pcm);
            pcm = NULL;
        }
    }
}

int platform_audio_available(void) { return pcm != NULL; }

void platform_audio_service(void) {
    while (pcm && audio_count) {
        unsigned span = PCM_QUEUE - audio_head;
        unsigned count = audio_count < span ? audio_count : span;
        long written = alsa_writei(pcm, audio_queue[audio_head], count);
        if (written == -32 || written == -77) {  // -EPIPE / -ESTRPIPE
            written = alsa_recover(pcm, (int)written, 1);
            if (!written) written = alsa_writei(pcm, audio_queue[audio_head], count);
        }
        if (!written || written == -11 || written == -4) return;  // -EAGAIN / -EINTR
        if (written < 0) { audio_head = audio_count = 0; return; }
        audio_head = (audio_head + (unsigned)written) % PCM_QUEUE;
        audio_count -= (unsigned)written;
    }
}

void platform_audio_write(const int16_t *samples, unsigned frames) {
    if (!pcm || !samples || !frames) return;
    platform_audio_service();
    if (frames > PCM_QUEUE - audio_count) {
        // Queue full: drop the oldest samples to make room.
        unsigned trim = frames - (PCM_QUEUE - audio_count);
        unsigned queued_trim = trim < audio_count ? trim : audio_count;
        audio_head = (audio_head + queued_trim) % PCM_QUEUE;
        audio_count -= queued_trim;
        samples += (trim - queued_trim) * 2;
        frames -= trim - queued_trim;
    }
    unsigned tail = (audio_head + audio_count) % PCM_QUEUE;
    while (frames) {
        unsigned count = PCM_QUEUE - tail < frames ? PCM_QUEUE - tail : frames;
        memcpy(audio_queue[tail], samples, count * sizeof audio_queue[0]);
        samples += 2 * count;
        audio_count += count;
        frames -= count;
        tail = 0;
    }
    platform_audio_service();
}

// --- framebuffer ----------------------------------------------------------

int platform_open(int firmware_keys) {
    if (pipe2(stop_pipe, O_NONBLOCK | O_CLOEXEC)) return 0;
    signal_writer = stop_pipe[1];
    signal(SIGTERM, request_quit);
    signal(SIGINT, request_quit);

    struct fb_fix_screeninfo fixed;
    fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &video) ||
        ioctl(fb, FBIOGET_FSCREENINFO, &fixed)) return 0;
    original_y = video.yoffset;
    back = original_y ? 0 : Q2_WIDTH;
    stride = fixed.line_length / 4;
    if (video.xres != Q2_HEIGHT || video.yres != Q2_WIDTH ||
        video.yres_virtual < 2 * Q2_WIDTH || video.bits_per_pixel != 32 ||
        video.red.offset != 16 || video.green.offset != 8 ||
        video.blue.offset != 0 || stride < Q2_HEIGHT ||
        (size_t)2 * Q2_WIDTH * stride * 4 > fixed.smem_len) return 0;
    map_size = fixed.smem_len;
    map = (uint32_t *)mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (map == MAP_FAILED) { map = NULL; return 0; }

    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = INPUT_SOCKET };
    sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (sock < 0) return 0;
    unlink(INPUT_SOCKET);
    if (bind(sock, (struct sockaddr *)&address, sizeof address)) return 0;

    open_audio();
    firmware_buttons = firmware_keys;
    if (!firmware_buttons) grab_buttons();
    fprintf(stderr, "q2-atari: ALSA=%s GPIO=%s (input via firmware socket%s)\n",
            pcm ? "open" : "unavailable",
            raw_buttons ? "grabbed" : "using firmware fallback",
            raw_buttons ? ", GPIO fallback" : "");
    return 1;
}

uint32_t *platform_canvas(void) { return canvas; }

void platform_present(void) {
    // Rotate the 375x320 canvas 90 degrees into the 320x375 back page:
    // canvas (cx, cy) lands at framebuffer (HEIGHT-1-cy, cx).
    uint32_t *page = map + back * stride;
    for (int cy = 0; cy < Q2_HEIGHT; ++cy) {
        const uint32_t *src = canvas + cy * Q2_WIDTH;
        uint32_t *dst = page + (Q2_HEIGHT - 1 - cy);
        for (int cx = 0; cx < Q2_WIDTH; ++cx)
            dst[cx * stride] = src[cx];
    }
    video.yoffset = back;
    if (ioctl(fb, FBIOPAN_DISPLAY, &video) == 0)
        back = back ? 0 : Q2_WIDTH;
}

int platform_event(Q2Event *e) {
    // q2-boy's platform_event (platform.c): the grabbed GPIO device
    // is read first. EV_SYN/SYN_DROPPED becomes 'r' (clear all held
    // buttons); EV_KEY becomes 'g' with the Linux key code (Return
    // 103, Play 108, Previous 165, Next 163, Centre 116).
    struct input_event key;
    if (keys >= 0 && read(keys, &key, sizeof key) == sizeof key) {
        if (key.type == EV_SYN && key.code == SYN_DROPPED) {
            *e = (Q2Event){ 'r', 0, 0, 0 };
        } else if (key.type == EV_KEY && key.value <= 1) {
            *e = (Q2Event){ 'g', key.value ? 'd' : 'u', (int)key.code, 0 };
        } else {
            *e = (Q2Event){ 0 };
        }
        return 1;
    }
    char buffer[80];
    ssize_t n = sock < 0 ? -1 : recv(sock, buffer, sizeof buffer - 1, MSG_DONTWAIT);
    if (n < 1) return 0;
    buffer[n] = 0;
    *e = (Q2Event){ 0 };
    if (buffer[0] == 'w' && sscanf(buffer + 1, "%d", &e->x) == 1) {
        e->kind = 'w';
    } else if ((buffer[0] == 'k' || buffer[0] == 't') &&
               sscanf(buffer + 1, " %c %d %d", &e->action, &e->x, &e->y) >= 2) {
        e->kind = buffer[0];
    }
    // Anything else (e.g. 'v' volume answers) is ignored.
    return 1;
}

void platform_wait(uint64_t deadline_ns) {
    if (q2_quitting) return;
    struct pollfd input[] = {
        { keys, POLLIN, 0 }, { sock, POLLIN, 0 }, { stop_pipe[0], POLLIN, 0 },
    };
    struct timespec timeout, *limit = NULL;
    if (deadline_ns != UINT64_MAX) {
        uint64_t now = clock_ns();
        if (now >= deadline_ns) return;
        uint64_t delay = deadline_ns - now;
        timeout = (struct timespec){ (time_t)(delay / 1000000000ull),
                                     (long)(delay % 1000000000ull) };
        limit = &timeout;
    }
    (void)ppoll(input, sizeof input / sizeof input[0], limit, NULL);
}

void platform_close(void) {
    if (keys >= 0) { close(keys); keys = -1; raw_buttons = 0; }
    if (pcm) { alsa_drop(pcm); alsa_close(pcm); pcm = NULL; }
    if (alsa_handle) { dlclose(alsa_handle); alsa_handle = NULL; }
    if (sock >= 0) { close(sock); unlink(INPUT_SOCKET); }
    sock = -1;
    if (stop_pipe[1] >= 0) {
        signal_writer = -1;
        close(stop_pipe[1]);
        close(stop_pipe[0]);
        stop_pipe[0] = stop_pipe[1] = -1;
    }
    if (map) {
        video.yoffset = original_y;
        ioctl(fb, FBIOPAN_DISPLAY, &video);
        munmap(map, map_size);
        map = NULL;
    }
    if (fb >= 0) close(fb);
    fb = -1;
}
