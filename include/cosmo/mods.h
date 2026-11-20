/* Mod discovery, preferences, and raw asset replacements. */
#ifndef COSMO_MODS_H
#define COSMO_MODS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define COSMO_MOD_LIMIT 128
#define COSMO_MOD_TEXT  256
#define COSMO_CRT_MOD_ID "crt-shader"
#define COSMO_SCANLINES_MOD_ID "crt-scanlines"

enum cosmo_mod_feature {
    MOD_AURORA_PALETTE = 1u << 0,
    MOD_LOW_GRAVITY = 1u << 1,
    MOD_SHIELD_MODE = 1u << 2,
    MOD_TURBO_MODE = 1u << 3,
    MOD_WIDESCREEN = 1u << 4,
    MOD_MOTION_INTERPOLATION = 1u << 5,
    MOD_SMOOTH_GAMEPLAY = 1u << 6
};

typedef struct cosmo_mod {
    char id[64];
    char name[COSMO_MOD_TEXT];
    char version[64];
    char author[COSMO_MOD_TEXT];
    char description[COSMO_MOD_TEXT];
    char directory[1024];
    bool enabled;
    bool crt_shader;
    bool crt_scanlines;
    unsigned features;
    bool has_player_palette;
    uint8_t player_palette[16];
} cosmo_mod;

/* Discover Mods beside the executable and load Settings/mods.json under the
 * writable settings root. Invalid manifests are reported and skipped. */
bool mods_init(const char *program_dir, const char *settings_root);
size_t mods_count(void);
const cosmo_mod *mods_get(size_t index);
bool mods_set_enabled(size_t index, bool enabled);
bool mods_save(void);
const char *mods_display_filter_name(void);
bool mods_step_display_filter(int direction);
bool mods_gamepad_rumble_enabled(void);
bool mods_set_gamepad_rumble(bool enabled);

/* Whether any enabled mod requests the built-in CRT-style post-process. */
bool mods_crt_shader_enabled(void);
bool mods_crt_scanlines_enabled(void);
bool mods_feature_enabled(enum cosmo_mod_feature feature);
void mods_recolor_player(uint8_t *tiles, size_t size);

#define COSMO_PLAYER_COLOUR_COUNT 7
const char *mods_player_colour_name(int colour);
void mods_get_player_colours(int colours[2]);
void mods_preview_player_colours(uint8_t *tiles, size_t size, const int colours[2]);
/* Pass NULL to return to the enabled skin's colours. */
bool mods_save_player_colours(const int colours[2]);

/* Find an enabled mod's music/<asset>.mp3 replacement. */
bool mods_music_override(const char *asset_name, char *path, size_t path_size);
/* Find an enabled mod's sounds/<sound-id>.wav or .mp3 replacement. */
bool mods_sound_override(unsigned sound_number, char *path, size_t path_size);

/* Raw extracted replacement for an existing game asset, or zero on miss. */
size_t mods_read_override(const char *datadir, int episode, const char *asset_name,
                          void *dest, size_t max);

/* FILE-compatible replacement for the reconstructed game's original reader. */
FILE *mods_open_override(const char *datadir, int episode, const char *asset_name,
                         uint32_t *length);

#endif
