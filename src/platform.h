// SPDX-License-Identifier: GPL-2.0-or-later
// Q2 platform layer for q2-atari2600.
//
// The canvas is 375x320, the Q2's logical screen. The panel itself is
// 320x375, so presenting rotates the canvas 90 degrees into the
// framebuffer's back page and flips it in with FBIOPAN_DISPLAY.
// Audio goes through ALSA, loaded at runtime with dlopen so the binary
// does not need the firmware's libasound at link time. Input comes from
// the Q2's GPIO buttons (md-gpio-keys) and from the Unix socket the
// firmware feeds with wheel and touch events.
#ifndef Q2ATARI_PLATFORM_H
#define Q2ATARI_PLATFORM_H

#include <signal.h>
#include <stdint.h>

#define Q2_WIDTH 375
#define Q2_HEIGHT 320

#ifdef __cplusplus
extern "C" {
#endif

// One input event. kind 'g' is a GPIO key (action 'd' down / 'u' up,
// x holds the Linux key code). kind 'w' is a wheel turn (x is the
// signed tick count, positive is down). kind 'k' is a firmware key
// (action is the firmware's key character). kind 't' is a touch
// (x, y are screen coordinates).
typedef struct { char kind, action; int x, y; } Q2Event;

extern volatile sig_atomic_t q2_quitting;
extern int q2_raw_buttons;

int platform_open(void);
void platform_close(void);
int platform_event(Q2Event *event);
void platform_wait(uint64_t deadline_ns);
uint32_t *platform_canvas(void);
void platform_present(void);
uint64_t clock_ns(void);

// Audio. The sample rate is the core's output rate (20,933 Hz by
// default). platform_audio_write queues interleaved stereo s16 samples;
// a muted call or a missing audio device drops them silently.
void platform_audio_set_rate(unsigned rate);
void platform_audio_write(const int16_t *samples, unsigned frames);
void platform_audio_service(void);
int platform_audio_available(void);

#ifdef __cplusplus
}
#endif

#endif
