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

    snprintf(probe, sizeof probe, "%s/.cosmo-write-probe", dir);

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

        snprintf(src, sizeof src, "%s/COSMO%d.%s", from, episode,
                 GROUP_EXTENSIONS[i]);
        snprintf(dst, sizeof dst, "%s/COSMO%d.%s", to, episode,
                 GROUP_EXTENSIONS[i]);

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

    if (env && *env) {
        snprintf(slots[count++], MAX_PATH_LEN, "%s", env);
    }

    if (base) {
        /*
         * In a macOS bundle the executables are in Contents/MacOS and the data
         * is one level across, in Contents/Resources. SDL hands back a
         * trailing separator, which is trimmed so group.c can add its own.
         */
        snprintf(slots[count++], MAX_PATH_LEN, "%s../Resources", base);
        snprintf(slots[count++], MAX_PATH_LEN, "%s", base);
    }

    snprintf(slots[count++], MAX_PATH_LEN, ".");

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
            snprintf(data_dir, sizeof data_dir, "%s", shipped[i]);
            snprintf(write_dir, sizeof write_dir, "%s", shipped[i]);
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

    snprintf(data_dir, sizeof data_dir, "%s", prefs);
    {
        size_t len = strlen(data_dir);
        while (len > 1 && (data_dir[len - 1] == '/' || data_dir[len - 1] == '\\')) {
            data_dir[--len] = '\0';
        }
    }
    snprintf(write_dir, sizeof write_dir, "%s", data_dir);

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

    snprintf(folder, sizeof folder, "%.*s",
             (int)(slash - chosen_file), chosen_file);

    for (i = 0; i < SDL_arraysize(GROUP_EXTENSIONS); i++) {
        char src[MAX_PATH_LEN], dst[MAX_PATH_LEN];

        snprintf(src, sizeof src, "%s/COSMO%d.%s", folder, episode,
                 GROUP_EXTENSIONS[i]);
        snprintf(dst, sizeof dst, "%s/COSMO%d.%s", data_dir, episode,
                 GROUP_EXTENSIONS[i]);

        if (!copy_file(src, dst)) return false;
    }

    return group_episode_present(data_dir, episode);
}
