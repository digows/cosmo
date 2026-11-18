/* Mod discovery, preferences, and raw asset replacements. */
#ifndef COSMO_MODS_H
#define COSMO_MODS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define COSMO_MOD_LIMIT 128
#define COSMO_MOD_TEXT  256

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
} cosmo_mod;

/* Discover Mods beside the executable and load Settings/mods.json under the
 * writable settings root. Invalid manifests are reported and skipped. */
bool mods_init(const char *program_dir, const char *settings_root);
size_t mods_count(void);
const cosmo_mod *mods_get(size_t index);
bool mods_set_enabled(size_t index, bool enabled);
bool mods_save(void);

/* Whether any enabled mod requests the built-in CRT-style post-process. */
bool mods_crt_shader_enabled(void);
bool mods_crt_scanlines_enabled(void);

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
