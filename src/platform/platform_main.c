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
extern uint8_t scancodeWest, scancodeEast, scancodeNorth, scancodeSouth;
extern uint8_t scancodeJump, scancodeBomb;

#include "cosmo/audio.h"
#include "cosmo/dos_compat.h"
#include "cosmo/ega.h"
#include "cosmo/hardware.h"
#include "cosmo/input.h"
#include "cosmo/video.h"
#include "cosmo/paths.h"
#include "cosmo/mods.h"

#ifdef _WIN32
#   include <windows.h>
#endif

#define SCANCODE_QUEUE_SIZE 64

enum haptic_cue {
    HAPTIC_NONE = 0,
    HAPTIC_TYPEWRITER,
    HAPTIC_MENU_NAV,
    HAPTIC_MENU_BACK,
    HAPTIC_MENU_SELECT,
    HAPTIC_JUMP,
    HAPTIC_LAND,
    HAPTIC_TICK,
    HAPTIC_PICKUP,
    HAPTIC_BIG_PICKUP,
    HAPTIC_IMPACT,
    HAPTIC_BARREL_DESTROY,
    HAPTIC_HEAVY_IMPACT,
    HAPTIC_STOMP,
    HAPTIC_EXPLOSION,
    HAPTIC_HURT
};

/* Sound numbers from vendor/cosmore/src/sound.h. */
enum game_sound_number {
    SND_BIG_PRIZE = 1,
    SND_PLAYER_JUMP = 2,
    SND_PLAYER_LAND = 3,
    SND_PLAYER_HIT_HEAD = 5,
    SND_PLAYER_POUNCE = 6,
    SND_PLAYER_DEATH = 7,
    SND_DOOR_UNLOCK = 8,
    SND_EXPLOSION = 10,
    SND_BARREL_DESTROY_1 = 12,
    SND_PRIZE = 13,
    SND_PLAYER_HURT = 14,
    SND_ROAMER_GIFT = 17,
    SND_BIG_OBJECT_HIT = 27,
    SND_DESTROY_SOLID = 25,
    SND_PLACE_BOMB = 29,
    SND_TEXT_TYPEWRITER = 44,
    SND_SMASH = 50,
    SND_OBJECT_HIT = 37,
    SND_BOSS_DAMAGE = 55,
    SND_BARREL_DESTROY_2 = 61
};

static SDL_AtomicInt game_running;
static SDL_AtomicInt haptic_pending;
static uint16_t camera_scroll_x, camera_scroll_y;
static bool camera_backdrop_x, camera_backdrop_y;
static unsigned camera_frame_ticks;

static void request_haptic(int cue)
{
    if (cue != HAPTIC_NONE) {
        int pending = SDL_GetAtomicInt(&haptic_pending);
        while (pending < cue &&
               !SDL_CompareAndSwapAtomicInt(&haptic_pending, pending, cue)) {
            pending = SDL_GetAtomicInt(&haptic_pending);
        }
    }
}

FILE *platform_open_mod_asset(int episode, const char *asset_name,
                              uint32_t *length)
{
    return mods_open_override(paths_data_dir(),episode,asset_name,length);
}

int PlatformPlayMP3Music(char *asset_name)
{
    char path[1024];
    return mods_music_override(asset_name,path,sizeof path) &&
           audio_music_play_mp3(path);
}

void PlatformStopMP3Music(void)
{
    audio_music_stop_mp3();
}

int PlatformPlayMP3Sound(unsigned sound_number)
{
    char path[1024];

    audio_sound_stop_mp3();
    return mods_sound_override(sound_number,path,sizeof path) &&
           audio_sound_play_asset(path);
}

void PlatformHapticSound(unsigned sound_number)
{
    int cue = HAPTIC_NONE;
    switch (sound_number) {
    case SND_TEXT_TYPEWRITER: {
        /* Dialogue can request this for every character. Keep it tick-like. */
        static Uint64 last_typewriter_haptic;
        Uint64 now = SDL_GetTicks();
        if (!last_typewriter_haptic || now - last_typewriter_haptic >= 95) {
            cue = HAPTIC_TYPEWRITER;
            last_typewriter_haptic = now;
        }
        break;
    }
    case SND_PLAYER_JUMP:
        cue = HAPTIC_JUMP;
        break;
    case SND_PLAYER_LAND:
        cue = HAPTIC_LAND;
        break;
    case SND_PLAYER_HIT_HEAD:
    case SND_BIG_OBJECT_HIT:
    case SND_OBJECT_HIT:
    case SND_SMASH:
        cue = HAPTIC_IMPACT;
        break;
    case SND_BARREL_DESTROY_1:
    case SND_BARREL_DESTROY_2:
        cue = HAPTIC_BARREL_DESTROY;
        break;
    case SND_DESTROY_SOLID:
        cue = HAPTIC_HEAVY_IMPACT;
        break;
    case SND_DOOR_UNLOCK:
    case SND_PLACE_BOMB:
        cue = HAPTIC_TICK;
        break;
    case SND_PRIZE:
    case SND_ROAMER_GIFT:
        cue = HAPTIC_PICKUP;
        break;
    case SND_BIG_PRIZE:
        cue = HAPTIC_BIG_PICKUP;
        break;
    case SND_PLAYER_POUNCE:
    case SND_BOSS_DAMAGE:
        cue = HAPTIC_STOMP;
        break;
    case SND_PLAYER_HURT:
    case SND_PLAYER_DEATH:
        cue = HAPTIC_HURT;
        break;
    case SND_EXPLOSION:
        cue = HAPTIC_EXPLOSION;
        break;
    default:
        break;
    }

    request_haptic(cue);
}

void PlatformHapticMenuInput(uint8_t scancode)
{
    if (scancode == 0x01) {
        request_haptic(HAPTIC_MENU_BACK);
    } else if (scancode == 0x48 || scancode == 0x4B ||
               scancode == 0x4D || scancode == 0x50) {
        request_haptic(HAPTIC_MENU_NAV);
    } else {
        request_haptic(HAPTIC_MENU_SELECT);
    }
}

void PlatformStopMP3Sound(void)
{
    audio_sound_stop_mp3();
}

void PlatformSetAudioVolumes(int sound_percent, int music_percent)
{
    audio_set_volumes(sound_percent, music_percent);
}

const char *PlatformDisplayFilterName(void)
{
    return mods_display_filter_name();
}

struct display_filter_change {
    int direction;
    bool saved;
};

static void SDLCALL step_display_filter(void *userdata)
{
    struct display_filter_change *change = userdata;
    change->saved = mods_step_display_filter(change->direction);
}

int PlatformStepDisplayFilter(int direction)
{
    /* Keep filter writes on the thread that renders and reads the mod state. */
    struct display_filter_change change = {direction, false};
    return SDL_RunOnMainThread(step_display_filter, &change, true) && change.saved;
}

void PlatformSetMusicPaused(int paused)
{
    audio_set_music_paused(paused != 0);
}

int PlatformLowGravity(void)
{
    return mods_feature_enabled(MOD_LOW_GRAVITY);
}

int PlatformShieldMode(void)
{
    return mods_feature_enabled(MOD_SHIELD_MODE);
}

int PlatformTurboMode(void)
{
    return mods_feature_enabled(MOD_TURBO_MODE);
}

void PlatformRecolorPlayer(uint8_t *tiles, unsigned size)
{
    mods_recolor_player(tiles, size);
}

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
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_AC_BACK:      return 0x01;
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
static bool gamepad_action_held[9];
static uint8_t gamepad_action_code[9];
static SDL_Gamepad *gamepad;
static bool gamepad_scan_done;
static bool window_focused = true;
static bool trace_input;
static bool another_gamepad_action_holds(int action, uint8_t code);

static void release_key(SDL_Scancode sc)
{
    uint8_t code = sdl_to_xt(sc);

    if (code && !another_gamepad_action_holds(-1, code))
        queue_scancode((uint8_t)(code | 0x80));
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
/* Gamepad input                                                             */
/* ------------------------------------------------------------------------ */

static bool keyboard_holds_xt(uint8_t code)
{
    for (int sc = 0; sc < SDL_SCANCODE_COUNT; ++sc)
        if (key_held[sc] && sdl_to_xt((SDL_Scancode)sc) == code) return true;
    return false;
}

static bool another_gamepad_action_holds(int action, uint8_t code)
{
    for (int i = 0; i < (int)SDL_arraysize(gamepad_action_held); ++i)
        if (i != action && gamepad_action_held[i] && gamepad_action_code[i] == code)
            return true;
    return false;
}

static void gamepad_action(int action, uint8_t code, bool down)
{
    if (gamepad_action_held[action] &&
        (gamepad_action_code[action] != code || !down)) {
        uint8_t old = gamepad_action_code[action];
        gamepad_action_held[action] = false;
        if (!keyboard_holds_xt(old) && !another_gamepad_action_holds(action, old))
            queue_scancode((uint8_t)(old | 0x80));
    }
    if (down && code && !gamepad_action_held[action]) {
        gamepad_action_code[action] = code;
        gamepad_action_held[action] = true;
        if (trace_input)
            fprintf(stderr, "[input] gamepad action %d -> XT 0x%02x\n",
                    action, (unsigned)code);
        queue_scancode(code);
    }
}

static void release_gamepad_actions(void)
{
    for (int i = 0; i < (int)SDL_arraysize(gamepad_action_held); ++i)
        gamepad_action(i, gamepad_action_code[i], false);
}

static void gamepad_open_first(void)
{
    int count = 0;
    SDL_JoystickID *ids;
    if (gamepad || gamepad_scan_done) return;
    gamepad_scan_done = true;
    ids = SDL_GetGamepads(&count);
    if (!ids) return;
    for (int i = 0; i < count; ++i) {
        if (SDL_IsGamepad(ids[i])) {
            gamepad = SDL_OpenGamepad(ids[i]);
            if (gamepad) break;
        }
    }
    SDL_free(ids);
    if (gamepad) SDL_Log("Controller connected: %s", SDL_GetGamepadName(gamepad));
}

static void gamepad_close(void)
{
    release_gamepad_actions();
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad = NULL;
}

static bool gamepad_button(SDL_GamepadButton button)
{
    return gamepad && SDL_GetGamepadButton(gamepad, button);
}

static void update_gamepad(bool focused)
{
    bool west = false, east = false, north = false, south = false;
    if (!gamepad) gamepad_open_first();
    if (!gamepad || !focused) {
        release_gamepad_actions();
        return;
    }

    Sint16 x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    Sint16 y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    const Sint16 deadzone = 10000;
    west = x < -deadzone || SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    east = x > deadzone || SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    north = y < -deadzone || SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    south = y > deadzone || SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);

    gamepad_action(0, scancodeWest, west);
    gamepad_action(1, scancodeEast, east);
    gamepad_action(2, scancodeNorth, north);
    gamepad_action(3, scancodeSouth, south);
    gamepad_action(4, scancodeJump, gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH));
    gamepad_action(5, scancodeBomb, gamepad_button(SDL_GAMEPAD_BUTTON_EAST));
    /* A also confirms title and menu prompts. */
    gamepad_action(6, scancodeJump == 0x39 ? 0 : 0x39,
                   gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH));
    /* Start opens the same in-game menu as F1. */
    gamepad_action(7, 0x3B, gamepad_button(SDL_GAMEPAD_BUTTON_START));
    /* Left shoulder acts as Escape in menus. */
    gamepad_action(8, 0x01, gamepad_button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
}

static Uint16 boost_rumble(Uint16 strength)
{
    Uint32 boosted = ((Uint32)strength * 135u + 99u) / 100u;
    return (Uint16)(boosted > 65535u ? 65535u : boosted);
}

static void rumble_gamepad(Uint16 low_frequency, Uint16 high_frequency,
                           Uint32 duration_ms)
{
    if (gamepad) {
        SDL_RumbleGamepad(gamepad, boost_rumble(low_frequency),
                          boost_rumble(high_frequency), duration_ms);
    }
}

/* Called from the SDL main thread after reading requests from the game thread. */
static void play_pending_haptic(void)
{
    int cue = SDL_SetAtomicInt(&haptic_pending, HAPTIC_NONE);

    if (!mods_gamepad_rumble_enabled() || !gamepad || cue == HAPTIC_NONE) return;

    switch (cue) {
    case HAPTIC_TYPEWRITER:
        rumble_gamepad(5200, 9000, 16);
        break;
    case HAPTIC_MENU_NAV:
        rumble_gamepad(14000, 9500, 40);
        break;
    case HAPTIC_MENU_BACK:
        rumble_gamepad(10500, 7200, 32);
        break;
    case HAPTIC_MENU_SELECT:
        rumble_gamepad(30000, 22000, 70);
        break;
    case HAPTIC_JUMP:
        rumble_gamepad(5000, 3200, 28);
        break;
    case HAPTIC_LAND:
        rumble_gamepad(8000, 3200, 38);
        break;
    case HAPTIC_TICK:
        rumble_gamepad(5200, 6200, 34);
        break;
    case HAPTIC_PICKUP:
        rumble_gamepad(14500, 8800, 72);
        break;
    case HAPTIC_BIG_PICKUP:
        rumble_gamepad(23000, 14500, 92);
        break;
    case HAPTIC_IMPACT:
        rumble_gamepad(16500, 7000, 58);
        break;
    case HAPTIC_BARREL_DESTROY:
        rumble_gamepad(32000, 18000, 180);
        break;
    case HAPTIC_HEAVY_IMPACT:
        rumble_gamepad(45000, 27000, 180);
        break;
    case HAPTIC_STOMP:
        rumble_gamepad(28500, 18500, 108);
        break;
    case HAPTIC_EXPLOSION:
        rumble_gamepad(38000, 46000, 220);
        break;
    case HAPTIC_HURT:
        rumble_gamepad(52000, 36000, 260);
        break;
    default:
        break;
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
    video_capture_motion_frame(camera_scroll_x, camera_scroll_y,
                               camera_backdrop_x, camera_backdrop_y, camera_frame_ticks);
}

void PlatformSetCameraPosition(uint16_t scroll_x, uint16_t scroll_y,
                               int backdrop_x, int backdrop_y, uint16_t frame_ticks)
{
    camera_scroll_x = scroll_x;
    camera_scroll_y = scroll_y;
    camera_backdrop_x = backdrop_x != 0;
    camera_backdrop_y = backdrop_y != 0;
    camera_frame_ticks = frame_ticks;
}

void PlatformSetMotionInterpolationPaused(int paused)
{
    video_pause_motion_interpolation(paused != 0);
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
    char *game_argv[] = {data, NULL};

    /* paths_init() sets the working directory for the original DOS code.
     * An extra argument would be interpreted as a separate save directory. */
    InnerMain(1, game_argv);

    /*
     * The game finishing on its own and the game dying leave the same thing
     * behind -- a log that stops -- unless one of them says so. This is what
     * tells the two apart afterwards.
     */
    printf("[cosmo] the game returned; shutting down\n");
    fflush(stdout);

    SDL_SetAtomicInt(&game_running, 0);
    return 0;
}

/*
 * Keep PIT delivery independent of display presentation. VSync may wait for
 * the next refresh (8.3 ms on a 120 Hz display), which is too long to service
 * the game's 560 Hz timer on the presentation thread.
 */
static int SDLCALL timer_thread(void *unused)
{
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t next_tick = now;
    uint64_t next_debug = now;
    uint64_t tick_attempts = 0, tick_delivered = 0;
    bool debug_enabled = SDL_getenv("COSMO_DEBUG") != NULL;

    (void)unused;
    while (SDL_GetAtomicInt(&game_running)) {
        double hz = pit_frequency();
        uint64_t period = (uint64_t)((double)freq / (hz > 0.0 ? hz : 18.2));

        if (period == 0) period = 1;
        now = SDL_GetPerformanceCounter();

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
            /* Bound catch-up after a scheduler stall. */
            int budget = 8;
            while (now >= next_tick && budget--) {
                tick_attempts++;
                if (interrupt_deliver(8)) tick_delivered++;
                next_tick += period;
            }
            if (now >= next_tick) next_tick = now + period;
        }

        SDL_DelayNS(250000);
    }
    return 0;
}

/*
 * Give the program somewhere to say things.
 *
 * On Windows the game is built for the GUI subsystem, so that no console window
 * sits behind it -- which also means it starts with no stdout and no stderr at
 * all. A crash left nothing behind and there was no way to ask what happened.
 *
 * So: if it was started from a terminal, borrow that terminal. Otherwise write
 * to a file beside the saves, where it can be found afterwards. COSMO_LOG names
 * the file directly, on any platform.
 */
static void logging_init(void)
{
    const char *requested = SDL_getenv("COSMO_LOG");
    char path[1024];

    if (requested && *requested) {
        if (freopen(requested, "w", stdout)) {
            setvbuf(stdout, NULL, _IOLBF, 0);
            if (freopen(requested, "a", stderr)) {
                setvbuf(stderr, NULL, _IONBF, 0);
            }
        }
        return;
    }

#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        setvbuf(stdout, NULL, _IOLBF, 0);
        return;
    }

    if (snprintf(path, sizeof path, "%s/cosmo.log", paths_write_dir()) > 0 &&
        freopen(path, "w", stdout))
    {
        setvbuf(stdout, NULL, _IOLBF, 0);
        if (freopen(path, "a", stderr)) {
            setvbuf(stderr, NULL, _IONBF, 0);
        }
    }
#else
    (void)path;
#endif
}

#if defined(COSMO_EPISODE_MODULE)
int cosmo_episode_main(int argc, char *argv[])
#else
int main(int argc, char *argv[])
#endif
{
    SDL_Thread *thread;
    SDL_Thread *timer;
    uint64_t start_ms;

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

    logging_init();
    trace_input = SDL_getenv("COSMO_TRACE_INPUT") != NULL;
    if (trace_input) fprintf(stderr, "[input] tracing enabled\n");

    if (!mods_init(paths_install_dir(), paths_write_dir()))
        SDL_Log("Cosmo mods: initialization failed; continuing without mods");

    hardware_init();
    interrupts_init();
    ega_init();
    screenshots_configure();
    input_script_load(SDL_getenv("COSMO_SCRIPT"));
    audio_record_to(SDL_getenv("COSMO_AUDIO_WAV"));

    if (!video_init("Cosmo's Cosmic Adventure", 3,
                    mods_feature_enabled(MOD_WIDESCREEN))) return 1;
    video_set_motion_interpolation(
        mods_feature_enabled(MOD_MOTION_INTERPOLATION));
    if (argc > 1 && SDL_strcmp(argv[1], "--fullscreen") == 0)
        video_set_fullscreen(true);

    /* Silence is survivable; a missing audio device must not stop the game. */
    if (audio_init()) {
        /* Decode enabled sound replacements before the first gameplay cue. */
        for (unsigned sound = 1; sound <= COSMO_SOUND_ASSET_COUNT; ++sound) {
            char path[1024];
            if (mods_sound_override(sound, path, sizeof path))
                (void)audio_sound_preload_asset(path);
        }
    }

    SDL_SetAtomicInt(&game_running, 1);

    timer = SDL_CreateThread(timer_thread, "cosmo-pit", NULL);
    if (!timer) {
        SDL_SetAtomicInt(&game_running, 0);
        fprintf(stderr, "SDL_CreateThread (PIT): %s\n", SDL_GetError());
        return 1;
    }

    thread = SDL_CreateThread(game_thread, "cosmo-game", argv[0]);
    if (!thread) {
        SDL_SetAtomicInt(&game_running, 0);
        SDL_WaitThread(timer, NULL);
        fprintf(stderr, "SDL_CreateThread: %s\n", SDL_GetError());
        return 1;
    }

    start_ms = SDL_GetTicks();

    while (SDL_GetAtomicInt(&game_running)) {
        SDL_Event event;

        while (video_poll_event(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                /*
                 * The game thread may be parked in one of its busy-wait loops
                 * with no way to be told to stop, so closing the window ends
                 * the process outright rather than joining a thread that will
                 * never return.
                 */
                SDL_SetAtomicInt(&game_running, 0);
                SDL_WaitThread(timer, NULL);
                audio_shutdown();
                gamepad_close();
                video_shutdown();
                exit(EXIT_SUCCESS);
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                window_focused = false;
                release_all_keys();
                release_gamepad_actions();
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                window_focused = true;
            } else if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
                if (!gamepad) {
                    gamepad = SDL_OpenGamepad(event.gdevice.which);
                    if (gamepad)
                        SDL_Log("Controller connected: %s", SDL_GetGamepadName(gamepad));
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                if (gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
                    gamepad_close();
                    gamepad_scan_done = false;
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && trace_input) {
                const char *button = SDL_GetGamepadStringForButton((SDL_GamepadButton)event.gbutton.button);
                fprintf(stderr, "[input] SDL gamepad button %s (%u)\n",
                        button ? button : "unknown", (unsigned)event.gbutton.button);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                uint8_t code;

                if (event.key.scancode == SDL_SCANCODE_F12) {
                    if (event.key.mod & SDL_KMOD_SHIFT) {
                        video_start_frame_capture();
                        continue;
                    }
                    char path[64];
                    snprintf(path, sizeof path, "cosmo-%llu.png",
                             (unsigned long long)SDL_GetTicks());
                    if (video_write_png(path, 2)) printf("screenshot: %s\n", path);
                    continue;
                }

                code = sdl_to_xt(event.key.scancode);
                if (code) {
                    if (trace_input && (code == 0x01 || code == 0x10 || code == 0x1C))
                        fprintf(stderr, "[input] SDL keyboard %s -> XT 0x%02x\n",
                                SDL_GetScancodeName(event.key.scancode), (unsigned)code);
                    queue_scancode(code);
                    key_held[event.key.scancode] = true;
                }
            } else if (event.type == SDL_EVENT_KEY_UP) {
                release_key(event.key.scancode);
            }
        }

        update_gamepad(window_focused);
        play_pending_haptic();

        if (input_script_active()) {
            uint32_t elapsed = (uint32_t)(SDL_GetTicks() - start_ms);
            uint8_t code;

            while ((code = input_script_next(elapsed)) != 0) {
                queue_scancode(code);
            }
        }

        drain_scancodes();

        video_present();
        screenshots_update((uint32_t)(SDL_GetTicks() - start_ms));
        if (!input_script_active()) reconcile_held_keys();
    }

    SDL_WaitThread(thread, NULL);
    SDL_WaitThread(timer, NULL);
    audio_shutdown();
    gamepad_close();
    video_shutdown();
    return 0;
}
