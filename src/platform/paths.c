#include "cosmo/paths.h"
#include "cosmo/group.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#   include <direct.h>
#   define change_dir _chdir
#else
#   include <unistd.h>
#   define change_dir chdir
#endif

#define MAX_PATH_LEN 1024

static char data_dir[MAX_PATH_LEN];
static char write_dir[MAX_PATH_LEN];

/* The extensions an episode's two group files carry. */
static const char *const GROUP_EXTENSIONS[] = {"STN", "VOL"};

/*
 * Paths are assembled with explicit lengths rather than snprintf.
 *
 * A path built from another path cannot be shown to fit, because it need not:
 * the compiler is right to complain, and truncating one silently would mean
 * looking for the data somewhere other than where it was asked to. These say
 * what happens instead, which is that the caller is told and gives up.
 */
static bool path_copy(char *dest, size_t size, const char *text)
{
    size_t length = SDL_strlen(text);

    if (length >= size) return false;

    SDL_memcpy(dest, text, length + 1);
    return true;
}

static bool path_join(char *dest, size_t size, const char *dir, const char *leaf)
{
    size_t dir_length = SDL_strlen(dir);
    size_t leaf_length = SDL_strlen(leaf);

    if (dir_length + 1 + leaf_length >= size) return false;

    SDL_memcpy(dest, dir, dir_length);
    dest[dir_length] = '/';
    SDL_memcpy(dest + dir_length + 1, leaf, leaf_length + 1);
    return true;
}

/* The path of one of an episode's two group files. */
static bool group_path(char *dest, size_t size, const char *dir, int episode,
                       const char *extension)
{
    char leaf[16] = "COSMO0.";

    if (episode < 1 || episode > 9) return false;

    leaf[5] = (char)('0' + episode);
    if (SDL_strlcat(leaf, extension, sizeof leaf) >= sizeof leaf) return false;

    return path_join(dest, size, dir, leaf);
}

static bool dir_has_any_episode(const char *dir)
{
    int episode;

    for (episode = 1; episode <= 4; episode++) {
        if (group_episode_present(dir, episode)) return true;
    }
    return false;
}

/*
 * Whether a directory can be written to. Inside an application bundle it
 * cannot, or rather it should not: writing there breaks the signature and the
 * next update discards it.
 */
static bool dir_is_writable(const char *dir)
{
    char probe[MAX_PATH_LEN];
    SDL_IOStream *io;

    if (strstr(dir, ".app/Contents/")) return false;

    if (!path_join(probe, sizeof probe, dir, ".cosmo-write-probe")) return false;

    io = SDL_IOFromFile(probe, "wb");
    if (!io) return false;

    SDL_CloseIO(io);
    SDL_RemovePath(probe);
    return true;
}

static bool copy_file(const char *from, const char *to)
{
    size_t size = 0;
    void *data = SDL_LoadFile(from, &size);
    bool ok;

    if (!data) return false;

    ok = SDL_SaveFile(to, data, size);
    SDL_free(data);
    return ok;
}

/* Copy an episode's group files between directories, if they are not there. */
static void copy_episode(const char *from, const char *to, int episode)
{
    size_t i;

    if (group_episode_present(to, episode)) return;
    if (!group_episode_present(from, episode)) return;

    for (i = 0; i < SDL_arraysize(GROUP_EXTENSIONS); i++) {
        char src[MAX_PATH_LEN], dst[MAX_PATH_LEN];

        if (!group_path(src, sizeof src, from, episode, GROUP_EXTENSIONS[i]) ||
            !group_path(dst, sizeof dst, to, episode, GROUP_EXTENSIONS[i])) {
            return;
        }

        if (!copy_file(src, dst)) {
            fprintf(stderr, "cosmo: could not copy %s\n", src);
            return;
        }
    }
}

/*
 * The directories that might hold the data as shipped, most specific first.
 * `slots` must have room for four entries.
 */
static int shipped_locations(char slots[][MAX_PATH_LEN])
{
    const char *env = SDL_getenv("COSMO_DATA_DIR");
    const char *base = SDL_GetBasePath();
    int count = 0;

    if (env && *env && path_copy(slots[count], MAX_PATH_LEN, env)) {
        count++;
    }

    if (base) {
        /*
         * In a macOS bundle the executables are in Contents/MacOS and the data
         * is one level across, in Contents/Resources. SDL hands back a
         * trailing separator, which is trimmed so group.c can add its own.
         */
        if (path_join(slots[count], MAX_PATH_LEN, base, "../Resources")) count++;
        if (path_copy(slots[count], MAX_PATH_LEN, base)) count++;
    }

    if (path_copy(slots[count], MAX_PATH_LEN, ".")) count++;

    for (int i = 0; i < count; i++) {
        size_t len = strlen(slots[i]);
        while (len > 1 && (slots[i][len - 1] == '/' || slots[i][len - 1] == '\\')) {
            slots[i][--len] = '\0';
        }
    }

    return count;
}

bool paths_init(void)
{
    char shipped[4][MAX_PATH_LEN];
    int count = shipped_locations(shipped);
    const char *prefs;
    int i, episode;

    /* Used in place when the data sits somewhere that takes the saves too. */
    for (i = 0; i < count; i++) {
        if (dir_has_any_episode(shipped[i]) && dir_is_writable(shipped[i])) {
            if (!path_copy(data_dir, sizeof data_dir, shipped[i])) continue;
            if (!path_copy(write_dir, sizeof write_dir, shipped[i])) continue;
            return change_dir(data_dir) == 0;
        }
    }

    /*
     * Otherwise the user's own directory becomes the working copy, and
     * whatever shipped with the application is copied in once.
     */
    prefs = SDL_GetPrefPath("digows", "Cosmo");
    if (!prefs) {
        fprintf(stderr, "cosmo: no writable directory available (%s)\n",
                SDL_GetError());
        return false;
    }

    if (!path_copy(data_dir, sizeof data_dir, prefs)) return false;
    {
        size_t len = strlen(data_dir);
        while (len > 1 && (data_dir[len - 1] == '/' || data_dir[len - 1] == '\\')) {
            data_dir[--len] = '\0';
        }
    }
    if (!path_copy(write_dir, sizeof write_dir, data_dir)) return false;

    for (i = 0; i < count; i++) {
        for (episode = 1; episode <= 4; episode++) {
            copy_episode(shipped[i], data_dir, episode);
        }
    }

    if (!dir_has_any_episode(data_dir)) return false;

    return change_dir(data_dir) == 0;
}

const char *paths_data_dir(void)
{
    return data_dir;
}

const char *paths_write_dir(void)
{
    return write_dir;
}

bool paths_import_episode(const char *chosen_file, int episode)
{
    char folder[MAX_PATH_LEN];
    const char *slash;
    size_t i;

    if (!chosen_file) return false;

    /* Both group files are expected to sit together, whichever was picked. */
    slash = strrchr(chosen_file, '/');
#ifdef _WIN32
    {
        const char *back = strrchr(chosen_file, '\\');
        if (back && (!slash || back > slash)) slash = back;
    }
#endif
    if (!slash) return false;

    {
        size_t length = (size_t)(slash - chosen_file);
        if (length >= sizeof folder) return false;
        SDL_memcpy(folder, chosen_file, length);
        folder[length] = '\0';
    }

    for (i = 0; i < SDL_arraysize(GROUP_EXTENSIONS); i++) {
        char src[MAX_PATH_LEN], dst[MAX_PATH_LEN];

        if (!group_path(src, sizeof src, folder, episode, GROUP_EXTENSIONS[i]) ||
            !group_path(dst, sizeof dst, data_dir, episode, GROUP_EXTENSIONS[i])) {
            return false;
        }

        if (!copy_file(src, dst)) return false;
    }

    return group_episode_present(data_dir, episode);
}
