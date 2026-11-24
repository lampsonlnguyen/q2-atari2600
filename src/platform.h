// SPDX-License-Identifier: GPL-2.0-or-later
// Q2 platform layer for q2-atari2600.
//
// The canvas is 375x320, the Q2's logical screen. The panel itself is
// 320x375, so presenting rotates the canvas 90 degrees into the
// framebuffer's back page and flips it in with FBIOPAN_DISPLAY.
// Audio goes through ALSA, loaded at runtime with dlopen so the binary
// does not need the firmware's libasound at link time. Input comes from
// the Unix socket the firmware feeds with wheel, key and touch datagrams
// (/tmp/q2-atari.input.sock) — or, on firmware too old to forward keys,
// from the grabbed md-gpio-keys device (see platform_open). The game
// must NOT grab GPIO unconditionally, or the firmware stops seeing the
// presses and never sends the datagrams (q2-pico8 works the same way).
#ifndef Q2ATARI_PLATFORM_H
#define Q2ATARI_PLATFORM_H

#include <signal.h>
#include <stdint.h>

#define Q2_WIDTH 375
#define Q2_HEIGHT 320

#ifdef __cplusplus
extern "C" {
#endif

// One input event. kind 'w' is a wheel turn (x is the firmware's raw
// travel, 200 per turn, rising anticlockwise). kind 'k' is a firmware
// key (action 'd' down / 'u' up, x holds the AWTK key code: Return
// 170, Centre 218, Play 171, Prev/Next side buttons 222/223). kind
// 'g' is a raw GPIO key from the grabbed md-gpio-keys device (only
// when the firmware does not forward keys; x holds the Linux key
// code: Return 103, Centre 116, Play 108, Prev/Next 165/163).
// kind 'r' is an EV_SYN/SYN_DROPPED: clear all held buttons. kind
// 't' is a touch (action 'd' down / 'm' move / 'u' up, x, y are
// screen coordinates in the 375x320 landscape view).
typedef struct { char kind, action; int x, y; } Q2Event;

extern volatile sig_atomic_t q2_quitting;

// firmware_keys is true when /tmp/q2-addon.caps contains "gameview"
// (q2-boy's main.c:3588): the firmware forwards keys on the socket,
// so the game must not grab GPIO. False on old firmware: the game
// grabs md-gpio-keys and reads 'g' events instead.
int platform_open(int firmware_keys);
void platform_close(void);
int platform_event(Q2Event *event);
// Non-zero while the GPIO device is grabbed (q2-boy's raw_buttons):
// socket 'k' events are then ignored, like q2-boy's decode_key.
int platform_raw_buttons(void);
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
