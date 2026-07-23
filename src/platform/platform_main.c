/*
 * platform_main.c -- Entry point and the machine the game runs on.
 *
 * The split is deliberate: the main thread plays the part of the PC hardware
 * and the second thread plays the part of the CPU.
 *
 * Cosmo's main loop never yields. It busy-waits on gameTickCount, which its
 * own int 8 handler increments, and it reads keyboard state that its int 9
 * handler fills in. On real hardware those handlers fired underneath the
 * running program; here the main thread fires them at the rate the game
 * programmed into the PIT, while the game runs uninterrupted on its own
 * thread. Presentation and window events stay on the main thread, which is
 * where SDL requires them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

/*
 * glue.h is deliberately not included here. It defines `bool`, `true` and
 * `false` as the game's own 16-bit types, which collide with the C11 keywords
 * SDL's headers rely on. Only one symbol is needed from the game anyway.
 */
void InnerMain(int argc, char *argv[]);

/*
 * The game's own key and command state, for COSMO_DEBUG reporting. `bbool` is
 * a byte and `isKeyDown` is indexed by scancode.
 */
extern volatile uint8_t isKeyDown[];
extern uint8_t cmdWest, cmdEast, cmdNorth, cmdSouth, cmdJump, cmdBomb;
extern uint8_t isAdLibPresent;
extern unsigned long adlib_writes;

#include "cosmo/audio.h"
#include "cosmo/dos_compat.h"
#include "cosmo/ega.h"
#include "cosmo/hardware.h"
#include "cosmo/input.h"
#include "cosmo/video.h"
#include "cosmo/paths.h"

#define TARGET_PRESENT_HZ 60.0
#define SCANCODE_QUEUE_SIZE 64

static SDL_AtomicInt game_running;
static SDL_AtomicInt frame_pending;

static int argc_saved;
static char **argv_saved;

/* ------------------------------------------------------------------------ */
/* Keyboard translation                                                      */
/* ------------------------------------------------------------------------ */

/*
 * SDL scancodes are USB HID usage IDs; the game expects the XT set the 8042
 * produced. This maps the keys Cosmo actually reads.
 */
static uint8_t sdl_to_xt(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_ESCAPE:       return 0x01;
    case SDL_SCANCODE_1:            return 0x02;
    case SDL_SCANCODE_2:            return 0x03;
    case SDL_SCANCODE_3:            return 0x04;
    case SDL_SCANCODE_4:            return 0x05;
    case SDL_SCANCODE_5:            return 0x06;
    case SDL_SCANCODE_6:            return 0x07;
    case SDL_SCANCODE_7:            return 0x08;
    case SDL_SCANCODE_8:            return 0x09;
    case SDL_SCANCODE_9:            return 0x0A;
    case SDL_SCANCODE_0:            return 0x0B;
    case SDL_SCANCODE_MINUS:        return 0x0C;
    case SDL_SCANCODE_EQUALS:       return 0x0D;
    case SDL_SCANCODE_BACKSPACE:    return 0x0E;
    case SDL_SCANCODE_TAB:          return 0x0F;
    case SDL_SCANCODE_Q:            return 0x10;
    case SDL_SCANCODE_W:            return 0x11;
    case SDL_SCANCODE_E:            return 0x12;
    case SDL_SCANCODE_R:            return 0x13;
    case SDL_SCANCODE_T:            return 0x14;
    case SDL_SCANCODE_Y:            return 0x15;
    case SDL_SCANCODE_U:            return 0x16;
    case SDL_SCANCODE_I:            return 0x17;
    case SDL_SCANCODE_O:            return 0x18;
    case SDL_SCANCODE_P:            return 0x19;
    case SDL_SCANCODE_LEFTBRACKET:  return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_RETURN:       return 0x1C;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
    /*
     * Command reports as Ctrl. The game's default jump key is Ctrl, but macOS
     * claims Control with Left, Right and Up for Mission Control, so the
     * combination the game needs most never reaches us. Command is not
     * spoken for, so it gives the player a jump key that works with the
     * arrows out of the box.
     */
    case SDL_SCANCODE_LGUI:
    case SDL_SCANCODE_RGUI:         return 0x1D;
    case SDL_SCANCODE_A:            return 0x1E;
    case SDL_SCANCODE_S:            return 0x1F;
    case SDL_SCANCODE_D:            return 0x20;
    case SDL_SCANCODE_F:            return 0x21;
    case SDL_SCANCODE_G:            return 0x22;
    case SDL_SCANCODE_H:            return 0x23;
    case SDL_SCANCODE_J:            return 0x24;
    case SDL_SCANCODE_K:            return 0x25;
    case SDL_SCANCODE_L:            return 0x26;
    case SDL_SCANCODE_SEMICOLON:    return 0x27;
    case SDL_SCANCODE_APOSTROPHE:   return 0x28;
    case SDL_SCANCODE_GRAVE:        return 0x29;
    case SDL_SCANCODE_LSHIFT:       return 0x2A;
    case SDL_SCANCODE_BACKSLASH:    return 0x2B;
    case SDL_SCANCODE_Z:            return 0x2C;
    case SDL_SCANCODE_X:            return 0x2D;
    case SDL_SCANCODE_C:            return 0x2E;
    case SDL_SCANCODE_V:            return 0x2F;
    case SDL_SCANCODE_B:            return 0x30;
    case SDL_SCANCODE_N:            return 0x31;
    case SDL_SCANCODE_M:            return 0x32;
    case SDL_SCANCODE_COMMA:        return 0x33;
    case SDL_SCANCODE_PERIOD:       return 0x34;
    case SDL_SCANCODE_SLASH:        return 0x35;
    case SDL_SCANCODE_RSHIFT:       return 0x36;
    case SDL_SCANCODE_KP_MULTIPLY:  return 0x37;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT:         return 0x38;
    case SDL_SCANCODE_SPACE:        return 0x39;
    case SDL_SCANCODE_CAPSLOCK:     return 0x3A;
    case SDL_SCANCODE_F1:           return 0x3B;
    case SDL_SCANCODE_F2:           return 0x3C;
    case SDL_SCANCODE_F3:           return 0x3D;
    case SDL_SCANCODE_F4:           return 0x3E;
    case SDL_SCANCODE_F5:           return 0x3F;
    case SDL_SCANCODE_F6:           return 0x40;
    case SDL_SCANCODE_F7:           return 0x41;
    case SDL_SCANCODE_F8:           return 0x42;
    case SDL_SCANCODE_F9:           return 0x43;
    case SDL_SCANCODE_F10:          return 0x44;
    case SDL_SCANCODE_HOME:         return 0x47;
    case SDL_SCANCODE_UP:           return 0x48;
    case SDL_SCANCODE_PAGEUP:       return 0x49;
    case SDL_SCANCODE_LEFT:         return 0x4B;
    case SDL_SCANCODE_RIGHT:        return 0x4D;
    case SDL_SCANCODE_END:          return 0x4F;
    case SDL_SCANCODE_DOWN:         return 0x50;
    case SDL_SCANCODE_PAGEDOWN:     return 0x51;
    case SDL_SCANCODE_INSERT:       return 0x52;
    case SDL_SCANCODE_DELETE:       return 0x53;
    default:                        return 0x00;
    }
}

/*
 * Scancodes wait here when the game has interrupts masked. Dropping them
 * instead would lose keystrokes during the brief windows when the game is
 * installing a vector.
 */
static uint8_t scancode_queue[SCANCODE_QUEUE_SIZE];
static int queue_head, queue_tail;

static void queue_scancode(uint8_t code)
{
    int next = (queue_tail + 1) % SCANCODE_QUEUE_SIZE;

    if (next == queue_head) return;  /* full; the oldest keystrokes win */
    scancode_queue[queue_tail] = code;
    queue_tail = next;
}

/*
 * Which physical keys we have reported as held. macOS does not always deliver
 * a key-up: holding Command suppresses them for other keys, and switching away
 * from the window drops them entirely. Either way the game would be left
 * believing a key is still down and Cosmo would walk into a wall forever.
 * Reconciling against SDL's own view once a frame catches all of it.
 */
static bool key_held[SDL_SCANCODE_COUNT];

static void release_key(SDL_Scancode sc)
{
    uint8_t code = sdl_to_xt(sc);

    if (code) queue_scancode((uint8_t)(code | 0x80));
    key_held[sc] = false;
}

static void reconcile_held_keys(void)
{
    const bool *state = SDL_GetKeyboardState(NULL);

    for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++) {
        if (key_held[sc] && !state[sc]) release_key((SDL_Scancode)sc);
    }
}

static void release_all_keys(void)
{
    for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++) {
        if (key_held[sc]) release_key((SDL_Scancode)sc);
    }
}

static void drain_scancodes(void)
{
    while (queue_head != queue_tail) {
        keyboard_push_scancode(scancode_queue[queue_head]);

        if (!interrupt_deliver(9)) return;  /* masked; try again next pass */

        queue_head = (queue_head + 1) % SCANCODE_QUEUE_SIZE;
    }
}

/* ------------------------------------------------------------------------ */
/* Presentation                                                              */
/* ------------------------------------------------------------------------ */

/*
 * Called from the game thread when it flips display pages. Presentation itself
 * has to happen on the main thread, so this only raises a flag.
 */
void platform_page_flipped(void)
{
    SDL_SetAtomicInt(&frame_pending, 1);
}

/* ------------------------------------------------------------------------ */
/* Timed screenshots                                                         */
/* ------------------------------------------------------------------------ */

/*
 * COSMO_SHOT_PATH and COSMO_SHOT_MS ask for screenshots at fixed moments after
 * startup, e.g. COSMO_SHOT_MS=500,3000. Driving this from the environment
 * rather than argv keeps it clear of the command line the game parses itself,
 * and gives automated comparison against DOSBox something to hook into.
 */
#define MAX_SHOTS 16

static uint32_t shot_times[MAX_SHOTS];
static int shot_count;
static int shot_next;
static const char *shot_prefix;

static void screenshots_configure(void)
{
    const char *times = SDL_getenv("COSMO_SHOT_MS");

    shot_prefix = SDL_getenv("COSMO_SHOT_PATH");
    if (!shot_prefix || !times) return;

    while (*times && shot_count < MAX_SHOTS) {
        char *end;
        long value = strtol(times, &end, 10);

        if (end == times) break;
        if (value >= 0) shot_times[shot_count++] = (uint32_t)value;

        times = (*end == ',') ? end + 1 : end;
    }
}

static void screenshots_update(uint32_t elapsed_ms)
{
    while (shot_next < shot_count && elapsed_ms >= shot_times[shot_next]) {
        char path[512];

        snprintf(path, sizeof path, "%s-%ums.png",
                 shot_prefix, shot_times[shot_next]);

        if (video_write_png(path, 2)) {
            printf("screenshot: %s\n", path);
            fflush(stdout);
        }

        shot_next++;
    }
}

/* ------------------------------------------------------------------------ */
/* Threads                                                                   */
/* ------------------------------------------------------------------------ */

static int SDLCALL game_thread(void *data)
{
    (void)data;

    InnerMain(argc_saved, argv_saved);

    SDL_SetAtomicInt(&game_running, 0);
    return 0;
}

int main(int argc, char *argv[])
{
    SDL_Thread *thread;
    uint64_t freq, now, next_tick, next_frame, start_ms, next_debug;
    uint64_t tick_attempts = 0, tick_delivered = 0;
    bool debug_enabled = SDL_getenv("COSMO_DEBUG") != NULL;

    argc_saved = argc;
    argv_saved = argv;

    /*
     * Settles where the group files are and moves there, so every bare
     * filename in the 1992 code resolves the way it did on DOS. The launcher
     * has usually done this already; doing it again is harmless and covers
     * running an episode program directly.
     */
    if (!paths_init()) {
        static const char *message =
            "No episode data found.\n\n"
            "COSMO1.STN and COSMO1.VOL must be beside the program, inside the "
            "application bundle, or in the folder it was started from.";

        fprintf(stderr, "cosmo: %s\n", message);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                 "Cosmo's Cosmic Adventure", message, NULL);
        return 1;
    }

    hardware_init();
    interrupts_init();
    ega_init();
    screenshots_configure();
    input_script_load(SDL_getenv("COSMO_SCRIPT"));
    audio_record_to(SDL_getenv("COSMO_AUDIO_WAV"));

    if (!video_init("Cosmo's Cosmic Adventure", 3)) return 1;

    /* Silence is survivable; a missing audio device must not stop the game. */
    audio_init();

    SDL_SetAtomicInt(&game_running, 1);

    thread = SDL_CreateThread(game_thread, "cosmo-game", NULL);
    if (!thread) {
        fprintf(stderr, "SDL_CreateThread: %s\n", SDL_GetError());
        return 1;
    }

    start_ms = SDL_GetTicks();
    freq = SDL_GetPerformanceFrequency();
    now = SDL_GetPerformanceCounter();
    next_tick = now;
    next_frame = now;
    next_debug = now;

    while (SDL_GetAtomicInt(&game_running)) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                /*
                 * The game thread may be parked in one of its busy-wait loops
                 * with no way to be told to stop, so closing the window ends
                 * the process outright rather than joining a thread that will
                 * never return.
                 */
                audio_shutdown();
                video_shutdown();
                exit(EXIT_SUCCESS);
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                release_all_keys();
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                uint8_t code;

                if (event.key.scancode == SDL_SCANCODE_F12) {
                    char path[64];
                    snprintf(path, sizeof path, "cosmo-%llu.png",
                             (unsigned long long)SDL_GetTicks());
                    if (video_write_png(path, 2)) printf("screenshot: %s\n", path);
                    continue;
                }

                code = sdl_to_xt(event.key.scancode);
                if (code) {
                    queue_scancode(code);
                    key_held[event.key.scancode] = true;
                }
            } else if (event.type == SDL_EVENT_KEY_UP) {
                uint8_t code = sdl_to_xt(event.key.scancode);
                if (code) {
                    queue_scancode((uint8_t)(code | 0x80));
                    key_held[event.key.scancode] = false;
                }
            }
        }

        if (input_script_active()) {
            uint32_t elapsed = (uint32_t)(SDL_GetTicks() - start_ms);
            uint8_t code;

            while ((code = input_script_next(elapsed)) != 0) {
                queue_scancode(code);
            }
        }

        drain_scancodes();

        now = SDL_GetPerformanceCounter();

        /*
         * Fire the timer interrupt at whatever rate the game programmed. It
         * asks for 140 Hz without an AdLib and 560 Hz with one, and the game
         * clock is derived entirely from how often this lands.
         */
        {
            double hz = pit_frequency();
            uint64_t period = (uint64_t)((double)freq / (hz > 0.0 ? hz : 18.2));

            if (period == 0) period = 1;

            if (debug_enabled && now >= next_debug) {
                fprintf(stderr,
                        "[cosmo] pit=%u (%.1f Hz) adlib=%u oplwrites=%lu int8 fired=%llu delivered=%llu"
                        " | keys space=%u ctrl=%u alt=%u left=%u right=%u"
                        " | cmd jump=%u west=%u east=%u bomb=%u\n",
                        pit_divisor(), hz, (unsigned)isAdLibPresent, adlib_writes,
                        (unsigned long long)tick_attempts,
                        (unsigned long long)tick_delivered,
                        isKeyDown[0x39], isKeyDown[0x1D], isKeyDown[0x38],
                        isKeyDown[0x4B], isKeyDown[0x4D],
                        cmdJump, cmdWest, cmdEast, cmdBomb);
                next_debug = now + freq;
            }

            if (now >= next_tick) {
                /* Never chase more than a few ticks; a stall must not turn
                 * into a burst that outruns the game thread. */
                int budget = 8;
                while (now >= next_tick && budget--) {
                    tick_attempts++;
                    if (interrupt_deliver(8)) tick_delivered++;
                    next_tick += period;
                }
                if (now >= next_tick) next_tick = now + period;
            }
        }

        if (now >= next_frame) {
            video_present();
            SDL_SetAtomicInt(&frame_pending, 0);
            next_frame = now + (uint64_t)((double)freq / TARGET_PRESENT_HZ);
            screenshots_update((uint32_t)(SDL_GetTicks() - start_ms));
            if (!input_script_active()) reconcile_held_keys();
        }

        SDL_DelayNS(250000);  /* 0.25 ms; keeps the loop from spinning hot */
    }

    SDL_WaitThread(thread, NULL);
    audio_shutdown();
    video_shutdown();
    return 0;
}
