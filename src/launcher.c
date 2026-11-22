/*
 * launcher.c -- Front door for the three episodes.
 *
 * Each episode differs by preprocessor conditionals that include or exclude
 * whole actor implementations. Windows loads the selected episode module into
 * this process and reuses this window.
 *
 * The menu is drawn with the game's own font on the game's own title art,
 * through the same emulated EGA the game uses, so it looks like part of the
 * game rather than part of the host.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#   define EPISODE_PROGRAM_SUFFIX ".exe"
#else
#   include <unistd.h>
#   define EPISODE_PROGRAM_SUFFIX ""
#endif

#include <SDL3/SDL.h>
#if defined(__ANDROID__) || defined(COSMO_IOS_APP)
#include <SDL3/SDL_main.h>
#endif
#ifdef __ANDROID__
#include <jni.h>
#endif

#include "cosmo/ega.h"
#include "cosmo/audio.h"
#include "cosmo/group.h"
#include "cosmo/font.h"
#include "cosmo/paths.h"
#include "cosmo/mods.h"
#include "cosmo/video.h"

#ifdef COSMO_HAVE_EPISODE4
#   define EPISODE_COUNT 4
#else
#   define EPISODE_COUNT 3
#endif
#define FULLSCREEN_IMAGE_SIZE 32000
#define MODS_MENU_NUMBER (EPISODE_COUNT + 1)

/*
 * Where the menu sits, in font tiles. Kept low so it does not
 * cover the logo it is sitting on. The screen is 25 rows, so this reaches the
 * bottom exactly.
 */
#define MENU_TOP     15
#define MENU_HEIGHT  10
#define MENU_LEFT     3
#define MENU_WIDTH   34
enum { TEXT_GRAY = 7, TEXT_GREEN = 10, TEXT_RED = 12 };
enum { MODS_LIST_TOP = 7, MODS_VISIBLE_ROWS = 11 };

/* The episode programs translate SDL gamepads to the game's key events, but
 * the launcher has its own menus and needs a small equivalent of that path. */
static SDL_Gamepad *launcher_gamepad;
static Uint64 launcher_next_gamepad_scan;
static bool launcher_gamepad_previous[6];
static Uint64 launcher_gamepad_repeat_at[4];
static bool launcher_audio_ready;
static int launcher_music_mode = -1;
static SDL_Thread *launcher_music_thread;
static SDL_AtomicInt launcher_music_result;

struct launcher_music_job {
    char path[1024];
};

static int SDLCALL load_launcher_remix(void *userdata)
{
    struct launcher_music_job *job = userdata;
    bool loaded = audio_music_play_mp3(job->path);
    free(job);
    SDL_SetAtomicInt(&launcher_music_result, loaded ? 1 : -1);
    return loaded ? 1 : 0;
}

static void launcher_poll_music_worker(void);

/* Preview original M2Easy by default, or its MP3 remix when selected. Keep the
 * audio device open while moving between launcher menus. */
static void launcher_sync_music(void)
{
    char path[1024];
    bool wants_remix = mods_music_override("MEASY2.MNI", path, sizeof path);
    int mode = wants_remix ? 1 : 0;

    if (!launcher_audio_ready || mode == launcher_music_mode) return;
    if (wants_remix) {
        if (launcher_music_thread) return;
        struct launcher_music_job *job = malloc(sizeof *job);
        if (!job) return;
        SDL_strlcpy(job->path, path, sizeof job->path);
        SDL_SetAtomicInt(&launcher_music_result, 0);
        launcher_music_thread = SDL_CreateThread(load_launcher_remix,
                                                  "cosmo-music-load", job);
        if (!launcher_music_thread) free(job);
    } else {
        size_t size = group_entry_size(paths_data_dir(), 1, "MEASY2.MNI");
        uint8_t *music = size ? malloc(size) : NULL;
        if (music && group_read(paths_data_dir(), 1, "MEASY2.MNI", music, size) == size &&
            audio_music_play_imf(music, size)) {
            launcher_music_mode = mode;
        } else {
            audio_music_stop_mp3();
            launcher_music_mode = -1;
            SDL_Log("Cosmo launcher: original M2Easy track is unavailable");
        }
        free(music);
    }
}

static void launcher_poll_music_worker(void)
{
    int result;

    if (!launcher_music_thread) return;
    result = SDL_GetAtomicInt(&launcher_music_result);
    if (!result || !SDL_CompareAndSwapAtomicInt(&launcher_music_result, result, 0)) return;
    SDL_WaitThread(launcher_music_thread, NULL);
    launcher_music_thread = NULL;

    if (result > 0) {
        char path[1024];
        if (mods_music_override("MEASY2.MNI", path, sizeof path)) {
            launcher_music_mode = 1;
        } else {
            /* A choice changed while decoding. Restore its current track. */
            launcher_music_mode = -1;
            launcher_sync_music();
        }
    } else {
        SDL_Log("Cosmo launcher: could not load the M2Easy remix");
    }
}

static void launcher_audio_shutdown(void)
{
    if (!launcher_audio_ready) return;
    if (launcher_music_thread) {
        SDL_WaitThread(launcher_music_thread, NULL);
        launcher_music_thread = NULL;
    }
    audio_shutdown();
    launcher_audio_ready = false;
    launcher_music_mode = -1;
}

static Uint16 launcher_boost_rumble(Uint16 strength)
{
    Uint32 boosted = ((Uint32)strength * 135u + 99u) / 100u;
    return (Uint16)(boosted > 65535u ? 65535u : boosted);
}

static void launcher_rumble(Uint16 low_frequency, Uint16 high_frequency,
                            Uint32 duration_ms)
{
    if (!mods_gamepad_rumble_enabled() || !launcher_gamepad ||
        !SDL_GamepadConnected(launcher_gamepad)) return;
    SDL_RumbleGamepad(launcher_gamepad,
                      launcher_boost_rumble(low_frequency),
                      launcher_boost_rumble(high_frequency), duration_ms);
}

static void launcher_menu_haptic(bool confirm)
{
    if (confirm) launcher_rumble(30000, 22000, 70);
    else launcher_rumble(14000, 9500, 40);
}

static bool launcher_poll_event(SDL_Event *event)
{
    if (!video_poll_event(event)) return false;
#ifdef COSMO_TOUCH_PREVIEW
    if (event->type == SDL_EVENT_FINGER_DOWN) {
        static const SDL_Keycode keys[TOUCH_BUTTON_COUNT] = {
            SDLK_LEFT, SDLK_RIGHT, SDLK_UP, SDLK_DOWN, SDLK_RETURN, SDLK_ESCAPE, SDLK_ESCAPE
        };
        int button = video_touch_button(event);
        if (button >= 0) {
            SDL_zero(*event);
            event->type = SDL_EVENT_KEY_DOWN;
            event->key.key = keys[button];
        }
    }
#endif
    return true;
}

static void push_menu_key(SDL_Keycode key)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = key;
    event.key.scancode = SDL_GetScancodeFromKey(key, NULL);
    event.key.down = true;
    event.key.repeat = false;
    SDL_PushEvent(&event);
}

static void launcher_poll_gamepad(void)
{
    static const SDL_Keycode directions[4] = {
        SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT
    };
    Uint64 now = SDL_GetTicks();

    if (launcher_gamepad && !SDL_GamepadConnected(launcher_gamepad)) {
        SDL_CloseGamepad(launcher_gamepad);
        launcher_gamepad = NULL;
        memset(launcher_gamepad_previous, 0, sizeof launcher_gamepad_previous);
    }

    if (!launcher_gamepad && now >= launcher_next_gamepad_scan) {
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        launcher_next_gamepad_scan = now + 500;
        if (ids) {
            for (int i = 0; i < count; ++i) {
                if (SDL_IsGamepad(ids[i])) {
                    launcher_gamepad = SDL_OpenGamepad(ids[i]);
                    if (launcher_gamepad) break;
                }
            }
            SDL_free(ids);
        }
    }

    if (launcher_gamepad) {
        Sint16 x = SDL_GetGamepadAxis(launcher_gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        Sint16 y = SDL_GetGamepadAxis(launcher_gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        bool held[6];
        const Sint16 deadzone = 10000;

        held[0] = y < -deadzone || SDL_GetGamepadButton(launcher_gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP);
        held[1] = y > deadzone || SDL_GetGamepadButton(launcher_gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        held[2] = x < -deadzone || SDL_GetGamepadButton(launcher_gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        held[3] = x > deadzone || SDL_GetGamepadButton(launcher_gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        held[4] = SDL_GetGamepadButton(launcher_gamepad, SDL_GAMEPAD_BUTTON_SOUTH);
        held[5] = SDL_GetGamepadButton(launcher_gamepad, SDL_GAMEPAD_BUTTON_EAST) ||
                  SDL_GetGamepadButton(launcher_gamepad, SDL_GAMEPAD_BUTTON_BACK) ||
                  SDL_GetGamepadButton(launcher_gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);

        for (int i = 0; i < 4; ++i) {
            if (held[i] && (!launcher_gamepad_previous[i] ||
                            now >= launcher_gamepad_repeat_at[i])) {
                push_menu_key(directions[i]);
                launcher_gamepad_repeat_at[i] = now + 180;
            }
        }
        if (held[4] && !launcher_gamepad_previous[4]) push_menu_key(SDLK_RETURN);
        if (held[5] && !launcher_gamepad_previous[5]) push_menu_key(SDLK_ESCAPE);
        memcpy(launcher_gamepad_previous, held, sizeof held);
    }
}

struct episode {
    int number;
    const char *title;
    bool present;
};

static struct episode episodes[EPISODE_COUNT] = {
    {1, "Forbidden Planet", false},
    {2, "Mad Scientist", false},
    {3, "Secret Lab", false},
#ifdef COSMO_HAVE_EPISODE4
    {4, "New Episode", false},
#endif
};



/* ------------------------------------------------------------------------ */
/* Drawing                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Upload a fullscreen image, one EGA plane at a time. This is
 * DrawFullscreenImage()'s loop from game1.c, without the fade.
 */
static bool draw_background(int episode)
{
    unsigned char image[FULLSCREEN_IMAGE_SIZE];
    unsigned mask = 0x0100;

    if (group_read(paths_data_dir(), episode, "PRETITLE.MNI", image, sizeof image)
            != FULLSCREEN_IMAGE_SIZE) {
        return false;
    }

    ega_out_word(EGA_PORT_GC_INDEX, (0x00 << 8) | GC_MODE);
    ega_out_word(EGA_PORT_GC_INDEX, (0xFF << 8) | GC_BIT_MASK);

    for (unsigned base = 0; base < FULLSCREEN_IMAGE_SIZE;
         base += EGA_PAGE_BYTES) {
        ega_out_word(EGA_PORT_SEQ_INDEX, (uint16_t)(SEQ_MAP_MASK | mask));

        for (unsigned i = 0; i < EGA_PAGE_BYTES; i++) {
            ega_write(i, image[i + base]);
        }
        mask <<= 1;
    }

    return true;
}

/*
 * The palette the game settles on after its fade-in: register N holds N, and
 * N + 8 from register 8 up.
 */
static void install_palette(void)
{
    unsigned skip = 0;

    for (unsigned reg = 0; reg < 16; reg++) {
        if (reg == 8) skip = 8;
        ega_set_palette_register((uint8_t)reg, (uint8_t)(reg + skip));
    }
}

static void unfold_launch_frame(void)
{
    int center_y = MENU_TOP + MENU_HEIGHT / 2 - 1;
    for (int width = 4; width < MENU_WIDTH; width += 4) {
        font_draw_frame((FONT_COLUMNS - width) / 2, center_y, width, 2);
        video_present();
        video_delay(12);
    }
    for (int height = 2; height < MENU_HEIGHT; height += 2) {
        font_draw_frame(MENU_LEFT, MENU_TOP + (MENU_HEIGHT - height) / 2,
                        MENU_WIDTH, height);
        video_present();
        video_delay(12);
    }
}

static void draw_menu(int selected)
{
    char line[64];
    int row = MENU_TOP + 3;
    const int cursor_x = MENU_LEFT + 2;
    const int title_x = MENU_LEFT + 4;
    const int status_x = MENU_LEFT + 24;

    font_draw_frame(MENU_LEFT, MENU_TOP, MENU_WIDTH, MENU_HEIGHT);

    font_draw_centered(MENU_TOP + 1, "Select an episode");

    /*
     * Every episode is listed, including the ones with no data. Hiding those
     * would hide the only route to installing them, and an episode marked as
     * missing is more use than an episode that is simply absent.
     */
    for (int i = 0; i < EPISODE_COUNT; i++) {
        const struct episode *episode = &episodes[i];
#ifdef COSMO_TOUCH_PREVIEW
        snprintf(line, sizeof line, "%s", episode->title);
#else
        snprintf(line, sizeof line, "%d) %s", episode->number, episode->title);
#endif
        if (i == selected) font_draw(cursor_x, row, ">");
        if (episode->present) {
            font_draw(title_x, row, line);
        } else {
            font_draw_colored(title_x, row, line, TEXT_GRAY);
            font_draw_colored(status_x, row, "Not found", TEXT_GRAY);
        }
        row++;
    }

    row = MENU_TOP + MENU_HEIGHT - 3;
    if (selected == EPISODE_COUNT) font_draw_colored(cursor_x, row, ">", TEXT_RED);
#ifdef COSMO_TOUCH_PREVIEW
    int customise_x = title_x;
#else
    snprintf(line, sizeof line, "%d)", MODS_MENU_NUMBER);
    font_draw_colored(title_x, row, line, TEXT_RED);
    int customise_x = title_x + (int)strlen(line) + 1;
#endif
    font_draw_colored(customise_x, row, "Customise", TEXT_GREEN);
}

struct mods_selector {
    const char *label;
    const char *description;
    const char *primary_id;
    const char *secondary_id;
    const char *values[2];
};

static const struct mods_selector mods_selectors[] = {
    {"Filter", "Choose CRT blur, scanlines, both, or a clean image. "
               "Applies in standard and widescreen modes.",
     COSMO_CRT_MOD_ID, COSMO_SCANLINES_MOD_ID, {NULL, NULL}},
    {"Soundtrack", "Choose the original music or the remixed soundtrack for Episode 1.",
     "mp3-music", NULL, {"Original", "New Remixed"}},
    {"SFX", "Choose the original sound effects or a softened mix.",
     "soft-sound-effects", NULL, {"Original", "Softened"}},
};

struct mods_row {
    const struct mods_selector *selector;
    size_t primary, secondary;
    bool rumble;
    bool touch;
};

#ifdef COSMO_TOUCH_PREVIEW
void PlatformLoadTouchControls(void);
int PlatformTouchControlsEnabled(void);
int PlatformToggleTouchControls(void);
#endif

static size_t collect_mods_rows(struct mods_row *rows)
{
    size_t count = mods_count();
    size_t used = 0;
    bool grouped[COSMO_MOD_LIMIT] = {false};
    rows[used++] = (struct mods_row){0};
#ifdef COSMO_TOUCH_PREVIEW
    rows[used++] = (struct mods_row){.touch = true};
#endif

    for (size_t i = 0; i < SDL_arraysize(mods_selectors); ++i) {
        const struct mods_selector *selector = &mods_selectors[i];
        struct mods_row row = {.selector = selector, .primary = SIZE_MAX, .secondary = SIZE_MAX};
        for (size_t j = 0; j < count; ++j) {
            const char *id = mods_get(j)->id;
            if (!SDL_strcasecmp(id, selector->primary_id)) {
                row.primary = j;
                grouped[j] = true;
            } else if (selector->secondary_id && !SDL_strcasecmp(id, selector->secondary_id)) {
                row.secondary = j;
                grouped[j] = true;
            }
        }
        if (row.primary != SIZE_MAX || row.secondary != SIZE_MAX) rows[used++] = row;
    }
    rows[used++] = (struct mods_row){.rumble = true};
    for (size_t i = 0; i < count; ++i) {
        if (!grouped[i]) rows[used++] = (struct mods_row){.primary = i};
    }
    return used;
}

static bool step_mods_choice(const struct mods_row *row, int step)
{
    if (row->selector->secondary_id) return mods_step_display_filter(step);
    return mods_set_enabled(row->primary, !mods_get(row->primary)->enabled);
}

static void draw_mod_description(const char *description)
{
    char line[35];
    for (int row = 19; row <= 22; ++row) {
        description += strspn(description, " \t\r\n");
        if (!*description) break;
        size_t length = SDL_min(strcspn(description, "\r\n"), sizeof line - 1);
        size_t end = length;
        if (description[end] && !strchr(" \t\r\n", description[end])) {
            while (end > 0 && !strchr(" \t", description[end])) --end;
            if (end == 0) end = length;
        }
        snprintf(line, sizeof line, "%.*s", (int)end, description);
        font_draw(3, row, line);
        description += end;
    }
}

static void draw_mods_menu(const struct mods_row *rows, size_t count,
                           size_t selected, size_t first, bool save_failed)
{
    size_t end = SDL_min(count, first + MODS_VISIBLE_ROWS);
    font_draw_frame(1, 4, 38, 21);
    font_draw_colored(15, 5, "Customise", TEXT_GREEN);

    for (size_t i = first; i < end; ++i) {
        int y = MODS_LIST_TOP + (int)(i - first);
        const struct mods_row *row = &rows[i];
        if (i == selected) font_draw(2, y, ">");
        if (row->selector) {
            char label[24];
            const char *value = row->selector->secondary_id ? mods_display_filter_name()
                : row->selector->values[mods_get(row->primary)->enabled];
            snprintf(label, sizeof label, "< %s >", value);
            font_draw(4, y, row->selector->label);
            font_draw(37 - (int)strlen(label), y, label);
        } else if (i == 0) {
            font_draw(4, y, "Character Colour");
#ifdef COSMO_TOUCH_PREVIEW
        } else if (row->touch) {
            bool enabled = PlatformTouchControlsEnabled();
            font_draw(4, y, "Touch Controls");
            font_draw_colored(34, y, enabled ? "ON" : "OFF", enabled ? TEXT_GREEN : TEXT_RED);
#endif
        } else {
            char name[29];
            const cosmo_mod *mod = row->rumble ? NULL : mods_get(row->primary);
            bool enabled = mod ? mod->enabled : mods_gamepad_rumble_enabled();
            snprintf(name, sizeof name, "%.28s", mod ? mod->name : "Gamepad Rumble");
            font_draw(4, y, name);
            font_draw_colored(34, y, enabled ? "ON" : "OFF", enabled ? TEXT_GREEN : TEXT_RED);
        }
    }
    if (first > 0) font_draw(36, MODS_LIST_TOP - 1, "\x18");
    if (end < count) font_draw(36, MODS_LIST_TOP + MODS_VISIBLE_ROWS, "\x19");

    const struct mods_row *row = &rows[selected];
    if (row->selector) {
        draw_mod_description(row->selector->description);
    } else if (selected == 0) {
        draw_mod_description("Change Cosmo's body and accents");
#ifdef COSMO_TOUCH_PREVIEW
    } else if (row->touch) {
        draw_mod_description("Show touch buttons during gameplay. Tap Select to toggle.");
#endif
    } else {
        draw_mod_description(row->rumble ? "Controller vibration in menus and gameplay."
                                        : mods_get(row->primary)->description);
    }
    if (save_failed) font_draw_centered(23, "Could not save mod settings");
}

static bool run_colour_menu(int episode);

static bool run_mods_menu(int episode)
{
    struct mods_row rows[COSMO_MOD_LIMIT + 3];
    size_t count = collect_mods_rows(rows);
    size_t selected = 0, first = 0;
    bool redraw = true, save_failed = false;
    while (true) {
        SDL_Event event;
        if (redraw) {
            draw_mods_menu(rows, count, selected, first, save_failed);
            redraw = false;
        }
        launcher_poll_music_worker();
        launcher_poll_gamepad();
        while (launcher_poll_event(&event)) {
            if (event.type == SDL_EVENT_QUIT) return true;
            if (event.type == SDL_EVENT_KEY_DOWN &&
                (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_AC_BACK)) {
                launcher_menu_haptic(false);
                return false;
            }
            if (event.type != SDL_EVENT_KEY_DOWN) continue;
            if (event.key.key == SDLK_UP) {
                selected = (selected + count - 1) % count;
                save_failed = false;
                launcher_menu_haptic(false);
            } else if (event.key.key == SDLK_DOWN) {
                selected = (selected + 1) % count;
                save_failed = false;
                launcher_menu_haptic(false);
            } else if (event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT) {
                if (!rows[selected].selector || event.key.repeat) continue;
                save_failed = !step_mods_choice(&rows[selected], event.key.key == SDLK_LEFT ? -1 : 1);
                if (!save_failed) launcher_sync_music();
                launcher_menu_haptic(!save_failed);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_SPACE) {
                if (event.key.repeat) continue;
                if (selected == 0) {
                    launcher_menu_haptic(true);
                    if (run_colour_menu(episode)) return true;
                    (void)draw_background(episode);
                } else if (rows[selected].selector) {
                    save_failed = !step_mods_choice(&rows[selected], 1);
                    if (!save_failed) launcher_sync_music();
                    launcher_menu_haptic(!save_failed);
                } else if (rows[selected].rumble) {
                    bool enabled = !mods_gamepad_rumble_enabled();
                    save_failed = !mods_set_gamepad_rumble(enabled);
                    if (!save_failed && !enabled && launcher_gamepad)
                        SDL_RumbleGamepad(launcher_gamepad, 0, 0, 0);
                    launcher_menu_haptic(!save_failed);
#ifdef COSMO_TOUCH_PREVIEW
                } else if (rows[selected].touch) {
                    save_failed = !PlatformToggleTouchControls();
                    launcher_menu_haptic(!save_failed);
#endif
                } else {
                    size_t index = rows[selected].primary;
                    save_failed = !mods_set_enabled(index, !mods_get(index)->enabled);
                    launcher_menu_haptic(!save_failed);
                }
            } else continue;
            if (selected < first) first = selected;
            if (selected >= first + MODS_VISIBLE_ROWS)
                first = selected - MODS_VISIBLE_ROWS + 1;
            redraw = true;
        }
        video_present();
        video_delay(16);
    }
}

/* The preview reads the east-facing stand frame from the installed episode. */
struct player_preview {
    unsigned width, height;
    uint8_t tiles[12 * 12 * 40];
};

static unsigned read_le16(const uint8_t *p)
{
    return p[0] | ((unsigned)p[1] << 8);
}

static struct player_preview load_player_preview(int episode)
{
    struct player_preview frame = {0};
    const char *data = paths_data_dir();
    uint8_t info[512];
    size_t info_size = mods_read_override(data, episode, "PLYRINFO.MNI", info, sizeof info);
    if (!info_size) info_size = group_read(data, episode, "PLYRINFO.MNI", info, sizeof info);
    if (info_size < 2) return frame;

    const unsigned stand_east = 23 + 4;
    size_t record = (read_le16(info) + stand_east * 4) * 2;
    if (record + 6 > info_size) return frame;

    unsigned height = read_le16(info + record);
    unsigned width = read_le16(info + record + 2);
    if (!width || !height || width > 12 || height > 12) return frame;

    size_t offset = read_le16(info + record + 4);
    size_t bytes = (size_t)width * height * 40;
    size_t total = group_entry_size(data, episode, "PLAYERS.MNI");
    if (offset + bytes > total) return frame;

    uint8_t *tiles = malloc(total);
    if (!tiles) return frame;
    size_t got = mods_read_override(data, episode, "PLAYERS.MNI", tiles, total);
    if (!got) got = group_read(data, episode, "PLAYERS.MNI", tiles, total);
    if (got == total) {
        memcpy(frame.tiles, tiles + offset, bytes);
        frame.width = width;
        frame.height = height;
    }
    free(tiles);
    return frame;
}

static void draw_player_preview(const struct player_preview *source, const int colours[2])
{
    if (!source->width) {
        font_draw(24, 12, "No preview");
        return;
    }

    enum { LEFT = 23, TOP = 6, WIDTH = 14, HEIGHT = 15 };
    /* The launcher draws on page zero. Fill an aligned rectangle in dark grey. */
    for (unsigned plane = 0; plane < 4; ++plane) {
        for (unsigned y = TOP * 8; y < (TOP + HEIGHT) * 8; ++y)
            memset(ega.vram[plane] + y * FONT_COLUMNS + LEFT, plane == 3 ? 255 : 0, WIDTH);
    }

    struct player_preview frame = *source;
    mods_preview_player_colours(frame.tiles, frame.width * frame.height * 40, colours);
    unsigned width = frame.width * 8, height = frame.height * 8;
    unsigned scale = SDL_min(3u, SDL_min(WIDTH * 8 / width, HEIGHT * 8 / height));
    unsigned left = LEFT * 8 + (WIDTH * 8 - width * scale) / 2;
    unsigned top = TOP * 8 + (HEIGHT * 8 - height * scale) / 2;

    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const uint8_t *row = frame.tiles + ((y / 8) * frame.width + x / 8) * 40 + (y % 8) * 5;
            unsigned bit = 0x80u >> (x % 8);
            if (row[0] & bit) continue;
            for (unsigned dy = 0; dy < scale; ++dy) {
                for (unsigned dx = 0; dx < scale; ++dx) {
                    unsigned screen_x = left + x * scale + dx;
                    size_t byte = (top + y * scale + dy) * FONT_COLUMNS + screen_x / 8;
                    uint8_t mask = (uint8_t)(0x80u >> (screen_x % 8));
                    for (unsigned plane = 0; plane < 4; ++plane) {
                        uint8_t *dst = &ega.vram[plane][byte];
                        if (row[1 + plane] & bit) *dst |= mask;
                        else *dst &= (uint8_t)~mask;
                    }
                }
            }
        }
    }
}

static bool run_colour_menu(int episode)
{
    struct player_preview preview = load_player_preview(episode);
    int colours[2], selected = 0;
    bool redraw = true, save_failed = false;
    mods_get_player_colours(colours);

    for (;;) {
        if (redraw) {
            char line[40];
            font_draw_frame(1, 3, 38, 22);
            font_draw_centered(4, "Character Colour");
            snprintf(line, sizeof line, "%c Body    <%s>", selected == 0 ? '>' : ' ',
                     mods_player_colour_name(colours[0]));
            font_draw(3, 9, line);
            snprintf(line, sizeof line, "%c Accents <%s>", selected == 1 ? '>' : ' ',
                     mods_player_colour_name(colours[1]));
            font_draw(3, 12, line);
            font_draw(3, 16, selected == 2 ? "> Reset colours" : "  Reset colours");
            draw_player_preview(&preview, selected == 2 ? NULL : colours);
            if (save_failed) font_draw_centered(23, "Could not save colours");
            redraw = false;
        }

        SDL_Event event;
        launcher_poll_music_worker();
        launcher_poll_gamepad();
        while (launcher_poll_event(&event)) {
            if (event.type == SDL_EVENT_QUIT) return true;
            if (event.type != SDL_EVENT_KEY_DOWN) continue;
            switch (event.key.key) {
            case SDLK_ESCAPE:
                launcher_menu_haptic(false);
                return false;
            case SDLK_UP:
            case SDLK_DOWN:
                selected = (selected + (event.key.key == SDLK_UP ? 2 : 1)) % 3;
                break;
            case SDLK_LEFT:
            case SDLK_RIGHT:
                if (selected == 2) continue;
                int step = event.key.key == SDLK_LEFT ? -1 : 1;
                colours[selected] = (colours[selected] + step + COSMO_PLAYER_COLOUR_COUNT)
                                    % COSMO_PLAYER_COLOUR_COUNT;
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE:
                if (event.key.repeat) continue;
                if (mods_save_player_colours(selected == 2 ? NULL : colours)) {
                    launcher_menu_haptic(true);
                    return false;
                }
                save_failed = true;
                redraw = true;
                continue;
            default:
                continue;
            }
            save_failed = false;
            redraw = true;
            launcher_menu_haptic(false);
        }
        video_present();
        video_delay(16);
    }
}

/* ------------------------------------------------------------------------ */
/* Selection                                                                */
/* ------------------------------------------------------------------------ */

static int first_present(void)
{
    for (int i = 0; i < EPISODE_COUNT; i++) {
        if (episodes[i].present) return i;
    }
    return -1;
}

/*
 * Every episode can be moved to, including the ones with no data. Choosing one
 * of those is how someone points the launcher at a copy they own, so refusing
 * to select them would hide the only way in.
 */
static int step_selection(int selected, int direction)
{
    return (selected + direction + EPISODE_COUNT + 1) % (EPISODE_COUNT + 1);
}

/* ------------------------------------------------------------------------ */
/* Installing an episode this port does not ship                             */
/* ------------------------------------------------------------------------ */

#ifdef __ANDROID__
static bool offer_import(int episode)
{
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    if (!env) return false;
    jobject activity = SDL_GetAndroidActivity();
    if (!activity) return false;

    jclass activity_class = (*env)->GetObjectClass(env, activity);
    jmethodID start = (*env)->GetMethodID(env, activity_class, "startEpisodeImport", "(I)V");
    jmethodID result = (*env)->GetMethodID(env, activity_class, "getEpisodeImportResult", "()I");
    int state = -1;
    if (start && result) {
        (*env)->CallVoidMethod(env, activity, start, (jint)episode);
        while (!(*env)->ExceptionCheck(env)) {
            state = (*env)->CallIntMethod(env, activity, result);
            if (state != 0) break;

            SDL_Event event;
            launcher_poll_music_worker();
            while (launcher_poll_event(&event)) {
                if (event.type == SDL_EVENT_QUIT) goto done;
            }
            SDL_Delay(16);
        }
    }

done:
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        state = -1;
    }
    (*env)->DeleteLocalRef(env, activity_class);
    (*env)->DeleteLocalRef(env, activity);
    return state == 1;
}
#else
static int  import_state;    /* 0 while the dialog is open, 1 done, -1 failed */
static int  import_episode;

static void SDLCALL import_chosen(void *userdata, const char *const *files,
                                  int filter)
{
    (void)userdata;
    (void)filter;

    if (!files || !*files) {          /* cancelled */
        import_state = -1;
        return;
    }

    import_state = paths_import_episode(*files, import_episode) ? 1 : -1;
}

/*
 * Explain what is missing, then let the file picker do the finding. Both group
 * files are copied, whichever of the two was chosen.
 */
static bool offer_import(int episode)
{
    static const SDL_DialogFileFilter filters[] = {
        {"Cosmo episode data", "stn;vol"},
        {"All files", "*"},
    };
    char message[512];

    snprintf(message, sizeof message,
             "Episode %d is not included.\n\n"
             "Only episode 1 is shareware. Episodes 2 and 3 were the paid "
             "ones and are still sold by Apogee.\n\n"
             "If you already own a copy, choose its COSMO%d.STN file on the "
             "next screen and it will be installed alongside episode 1.",
             episode, episode);

    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION,
                             "Cosmo's Cosmic Adventure", message,
                             video_get_window());

    import_episode = episode;
    import_state = 0;

    SDL_ShowOpenFileDialog(import_chosen, NULL, video_get_window(),
                           filters, SDL_arraysize(filters), NULL, false);

    /* The callback arrives on the main thread, so events have to keep moving. */
    while (import_state == 0) {
        SDL_Event event;
        launcher_poll_music_worker();
        while (launcher_poll_event(&event)) {
            if (event.type == SDL_EVENT_QUIT) return false;
        }
        SDL_Delay(16);
    }

    if (import_state < 0) return false;

    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION,
                             "Cosmo's Cosmic Adventure",
                             "Installed. It will be there next time too.",
                             video_get_window());
    return true;
}
#endif

/* Act on a menu choice, installing the episode first if it is not there. */
static int choose_episode(int selected)
{
    if (episodes[selected].present) return episodes[selected].number;

    if (!offer_import(episodes[selected].number)) return 0;

    episodes[selected].present =
        group_episode_present(paths_data_dir(), episodes[selected].number);

    return episodes[selected].present ? episodes[selected].number : 0;
}

/* Returns the chosen episode number, or 0 if the user quit. */
static int run_menu(void)
{
    int selected = first_present();
    bool redraw = true;

    for (;;) {
        SDL_Event event;

        if (redraw) {
            draw_menu(selected);
            redraw = false;

            /* Same capture hook the game has, so the menu can be checked
             * without a person looking at it. */
            {
                const char *shot = SDL_getenv("COSMO_SHOT_PATH");
                if (shot) {
                    char path[512];
                    snprintf(path, sizeof path, "%s-menu.png", shot);
                    video_write_png(path, 2);
                }
            }
        }

        launcher_poll_music_worker();
        launcher_poll_gamepad();
        while (launcher_poll_event(&event)) {
            if (event.type == SDL_EVENT_QUIT) return 0;

            if (event.type != SDL_EVENT_KEY_DOWN) continue;

            SDL_Keycode key = event.key.key;
            if (key == SDLK_AC_BACK) key = SDLK_ESCAPE;
            if (key >= SDLK_1 && key <= SDLK_6) {
                if (event.key.repeat) continue;
                int shortcut = (int)(key - SDLK_1) + 1;
                if (shortcut <= EPISODE_COUNT) selected = shortcut - 1;
                else if (shortcut == MODS_MENU_NUMBER) selected = EPISODE_COUNT;
                else continue;
                key = SDLK_RETURN;
            }

            switch (key) {
            case SDLK_ESCAPE:
            case SDLK_AC_BACK:
            case SDLK_Q:
                launcher_menu_haptic(false);
                return 0;

            case SDLK_UP:
            case SDLK_LEFT:
                selected = step_selection(selected, -1);
                launcher_menu_haptic(false);
                redraw = true;
                break;

            case SDLK_DOWN:
            case SDLK_RIGHT:
                selected = step_selection(selected, 1);
                launcher_menu_haptic(false);
                redraw = true;
                break;

            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE: {
                if (event.key.repeat) break;
                launcher_menu_haptic(true);
                if (selected == EPISODE_COUNT) {
                    int preview_episode = episodes[first_present()].number;
                    if (run_mods_menu(preview_episode)) return 0;
                    (void)draw_background(preview_episode);
                    install_palette();
                    ega_select_active_page(0);
                    redraw = true;
                    break;
                }
                int chosen = choose_episode(selected);
                if (chosen) return chosen;
                redraw = true;
                break;
            }

            default:
                break;
            }
        }

        video_present();
        video_delay(16);
    }
}

static void report_missing_data(void)
{
    static const char *message =
        "No episode data found.\n\n"
        "The game looks for COSMO1.STN and COSMO1.VOL beside the program, "
        "inside the application bundle, and in the folder it was started "
        "from.\n\n"
        "Episode 1 is shareware and freely available; episodes 2 and 3 are "
        "sold by Apogee.";

    fprintf(stderr, "cosmo: %s\n", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             "Cosmo's Cosmic Adventure", message, NULL);
}

/* ------------------------------------------------------------------------ */
/* Handing over                                                             */
/* ------------------------------------------------------------------------ */

/*
 * Replace this process with the episode's. Spawning it as a child instead
 * leaves the launcher holding a window and an event loop while the game tries
 * to open its own, which on macOS ends with neither getting the foreground.
 */
/*
 * The two spellings differ in more than name: Windows takes the argument
 * vector as const, POSIX does not.
 */
#if !defined(_WIN32) && !defined(__ANDROID__) && !defined(COSMO_IOS_APP) && \
    !defined(COSMO_VITA_APP)
static int replace_process(const char *program, char *argv[])
{
    return execv(program, argv);
}
#endif

static int run_episode(int number)
{
#if defined(__ANDROID__) || defined(COSMO_IOS_APP) || defined(COSMO_VITA_APP)
    extern int cosmo_episode_main(int argc, char *argv[]);
    char *episode_argv[] = {"cosmo", NULL};
    int (*episode_main)(int, char **) = cosmo_episode_main;
#ifdef __ANDROID__
    SDL_SharedObject *module = NULL;
    if (number == 2 || number == 3) {
        module = SDL_LoadObject(number == 2 ? "libcosmo2.so" : "libcosmo3.so");
        if (module)
            episode_main = (int (*)(int, char **))SDL_LoadFunction(module, "cosmo_episode_main");
        if (!module || !episode_main) {
            SDL_Log("Cosmo: could not load episode %d: %s", number, SDL_GetError());
            if (module) SDL_UnloadObject(module);
            return 1;
        }
    } else
#endif
    if (number != 1) return 1;
    /* Release the launcher's SDL window before the embedded episode creates
     * its own. Mobile platforms host both entry points in one process. */
    launcher_audio_shutdown();
    video_shutdown();
    int result = episode_main(1, episode_argv);
#ifdef __ANDROID__
    if (module) SDL_UnloadObject(module);
#endif
    return result;
#elif defined(_WIN32)
    static const char *const WHERE[] = {"episodes/", ""};
    char module_path[1024];
    char fullscreen_arg[] = "--fullscreen";
    char *module_argv[] = {module_path, NULL, NULL};
    SDL_PathInfo info;
    SDL_SharedObject *module;
    int (*episode_main)(int, char **) = NULL;
    size_t i;
    bool found = false;
    int argc = 1;
    int result;

    for (i = 0; i < SDL_arraysize(WHERE) && !found; i++) {
        int written = snprintf(module_path, sizeof module_path,
                               "%s/%scosmo%d.dll", paths_program_dir(),
                               WHERE[i], number);
        if (written < 0 || (size_t)written >= sizeof module_path) continue;
        found = SDL_GetPathInfo(module_path, &info);
    }

    if (!found) {
        fprintf(stderr, "cosmo: episode %d module is not installed beside "
                        "the launcher\n", number);
        return 1;
    }

    module = SDL_LoadObject(module_path);
    if (!module) {
        fprintf(stderr, "cosmo: cannot load %s: %s\n",
                module_path, SDL_GetError());
        return 1;
    }

    if (launcher_gamepad) {
        SDL_CloseGamepad(launcher_gamepad);
        launcher_gamepad = NULL;
    }

    episode_main = (int (*)(int, char **))SDL_LoadFunction(
        module, "cosmo_episode_main");
    if (!episode_main) {
        fprintf(stderr, "cosmo: %s has no episode entry point: %s\n",
                module_path, SDL_GetError());
        SDL_UnloadObject(module);
        return 1;
    }

    if (video_is_fullscreen()) {
        module_argv[1] = fullscreen_arg;
        argc = 2;
    }

    launcher_audio_shutdown();
    result = episode_main(argc, module_argv);
    SDL_UnloadObject(module);
    return result;
#else
    /*
     * The episode programs are looked for in a subdirectory first and beside
     * the launcher second.
     *
     * They cannot be merged into one executable: the episodes differ by
     * preprocessor conditionals that include or exclude whole actor
     * implementations, so each is genuinely different code with its own copy of
     * the game's globals. What can be done is to keep them out of the way, so
     * an unpacked folder shows one program to run rather than four.
     */
    static const char *const WHERE[] = {"episodes/", ""};
    char program[1024];
    char fullscreen_arg[] = "--fullscreen";
    char *child_argv[] = {program, NULL, NULL};
    SDL_PathInfo info;
    size_t i;
    bool found = false;

    for (i = 0; i < SDL_arraysize(WHERE) && !found; i++) {
        int written = snprintf(program, sizeof program, "%s/%scosmo%d%s",
                               paths_program_dir(), WHERE[i], number,
                               EPISODE_PROGRAM_SUFFIX);
        if (written < 0 || (size_t)written >= sizeof program) continue;

        found = SDL_GetPathInfo(program, &info);
    }

    if (!found) {
        fprintf(stderr, "cosmo: episode %d is not installed beside the "
                        "launcher\n", number);
        return 1;
    }

    /* Pass Windows' foreground permission to the episode before its new SDL
     * window is created. Without this, Windows may flash the taskbar button
     * instead of bringing the episode forward after the handoff. */
#ifdef _WIN32
    (void)AllowSetForegroundWindow(ASFW_ANY);
#endif

    /* Give up the window before the process image is replaced. */
    if (video_is_fullscreen()) child_argv[1] = fullscreen_arg;
    launcher_audio_shutdown();
    video_shutdown();

    /*
     * No write path is handed over. paths_init() has already made the working
     * directory both the source of the group files and somewhere the saves can
     * go, so the game's own relative paths mean the right thing -- which also
     * avoids JoinPath(), whose 80-byte static buffer joins with a DOS
     * separator and never terminates its result.
     */
    replace_process(program, child_argv);

    /* Only reached if the replacement failed. */
    fprintf(stderr, "cosmo: cannot start %s\n", program);
    return 1;
#endif
}

int main(int argc, char *argv[])
{
    int found = 0;
    int number = 0;

    /*
     * Before anything looks for a file. This settles where the data is and
     * moves there, so the 1992 code's bare filenames resolve the way they did
     * when a program started in the directory it lived in.
     */
    if (!paths_init()) {
        report_missing_data();
        return 1;
    }
    if (!mods_init(paths_install_dir(), paths_write_dir()))
        SDL_Log("Cosmo mods: initialization failed; continuing without mods");

    /*
     * An episode can be named outright, both to skip the menu and to keep the
     * launcher scriptable.
     */
    if (argc > 1) {
        long requested = strtol(argv[1], NULL, 10);
        if (requested >= 1 && requested <= EPISODE_COUNT) number = (int)requested;
    }

    for (int i = 0; i < EPISODE_COUNT; i++) {
        episodes[i].present = group_episode_present(paths_data_dir(), episodes[i].number);
        if (episodes[i].present) found++;
    }

    if (!found) {
        report_missing_data();
        return 1;
    }

    if (number == 0) {
        int background = episodes[first_present()].number;

        ega_init();
        ega_set_video_mode(0x0D);

#ifdef COSMO_TOUCH_PREVIEW
        PlatformLoadTouchControls();
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
        if (!video_init("Cosmo's Cosmic Adventure", 3, false)) return 1;
#ifdef COSMO_TOUCH_PREVIEW
        SDL_SetWindowFullscreen(video_get_window(), true);
        video_set_touch_controls(true);
        video_load_launcher_touch_texture();
#endif

        if (!font_load(paths_data_dir(), background) || !draw_background(background)) {
            fprintf(stderr, "cosmo: episode data is present but unreadable\n");
            video_shutdown();
            return 1;
        }

        install_palette();
        ega_select_active_page(0);
        unfold_launch_frame();
#ifdef COSMO_TOUCH_PREVIEW
        video_set_touch_context(TOUCH_MENU);
#endif
        launcher_audio_ready = audio_init();
        launcher_sync_music();

        number = run_menu();

        if (number == 0) {
            launcher_audio_shutdown();
            video_shutdown();
            return 0;
        }
    }

    return run_episode(number);
}
