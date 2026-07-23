/*
 * launcher.c -- Front door for the three episodes.
 *
 * Cosmo shipped as three programs, and this port builds three too. That is not
 * a packaging decision: the episodes differ by preprocessor conditionals that
 * include or exclude whole actor implementations, so they compile to genuinely
 * different code. See the comment in CMakeLists.txt.
 *
 * This finds which episodes have their data present and runs one, asking only
 * when there is something to ask about.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define EPISODE_COUNT 3

struct episode {
    int number;
    const char *title;
    bool present;
};

static struct episode episodes[EPISODE_COUNT] = {
    {1, "Episode 1 -- Forbidden Planet", false},
    {2, "Episode 2 -- Mad Scientist", false},
    {3, "Episode 3 -- Secret Lab", false},
};

/*
 * An episode is playable when its group files are where the game will look for
 * them, which is the working directory -- the same place the game itself
 * resolves COSMOn.STN and COSMOn.VOL.
 */
static bool episode_data_present(int number)
{
    char path[64];
    SDL_PathInfo info;

    snprintf(path, sizeof path, "COSMO%d.STN", number);
    if (!SDL_GetPathInfo(path, &info)) return false;

    snprintf(path, sizeof path, "COSMO%d.VOL", number);
    return SDL_GetPathInfo(path, &info);
}

static int count_present(void)
{
    int found = 0;

    for (int i = 0; i < EPISODE_COUNT; i++) {
        episodes[i].present = episode_data_present(episodes[i].number);
        if (episodes[i].present) found++;
    }
    return found;
}

/*
 * Ask which episode to play. A native dialog rather than something drawn in
 * the game's own style: this runs before any of the game's data is loaded, and
 * a launcher that has to boot an EGA to ask a question would be a strange way
 * around.
 */
static int choose_episode(void)
{
    SDL_MessageBoxButtonData buttons[EPISODE_COUNT + 1];
    SDL_MessageBoxData data;
    int count = 0;
    int chosen = -1;

    for (int i = 0; i < EPISODE_COUNT; i++) {
        if (!episodes[i].present) continue;

        buttons[count].flags = 0;
        buttons[count].buttonID = episodes[i].number;
        buttons[count].text = episodes[i].title;
        count++;
    }

    buttons[count].flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
    buttons[count].buttonID = -1;
    buttons[count].text = "Cancel";
    count++;

    memset(&data, 0, sizeof data);
    data.flags = SDL_MESSAGEBOX_INFORMATION;
    data.title = "Cosmo's Cosmic Adventure";
    data.message = "Which episode?";
    data.numbuttons = count;
    data.buttons = buttons;

    if (!SDL_ShowMessageBox(&data, &chosen)) {
        fprintf(stderr, "cosmo: %s\n", SDL_GetError());
        return -1;
    }

    return chosen;
}

static void report_missing_data(void)
{
    static const char *message =
        "No episode data found in this directory.\n\n"
        "Put COSMO1.STN and COSMO1.VOL (or the files for episodes 2 or 3)\n"
        "beside the game, then run it again.\n\n"
        "Episode 1 is shareware and freely available; episodes 2 and 3 are\n"
        "sold by Apogee. See gamedata/README.md.";

    fprintf(stderr, "cosmo: %s\n", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             "Cosmo's Cosmic Adventure", message, NULL);
}

int main(int argc, char *argv[])
{
    const char *base;
    char program[1024];
    int found;
    int number = 0;
    SDL_Process *child;
    const char *child_argv[3];

    /*
     * An episode can be named outright, both to skip the question and so the
     * per-episode binaries stay scriptable.
     */
    if (argc > 1) {
        long requested = strtol(argv[1], NULL, 10);
        if (requested >= 1 && requested <= EPISODE_COUNT) number = (int)requested;
    }

    found = count_present();

    if (found == 0) {
        report_missing_data();
        return 1;
    }

    if (number == 0) {
        if (found == 1) {
            /* Nothing worth asking about. */
            for (int i = 0; i < EPISODE_COUNT; i++) {
                if (episodes[i].present) number = episodes[i].number;
            }
        } else {
            number = choose_episode();
            if (number < 1) return 0;  /* cancelled */
        }
    }

    base = SDL_GetBasePath();
    if (!base) {
        fprintf(stderr, "cosmo: cannot locate the episode programs (%s)\n",
                SDL_GetError());
        return 1;
    }

    snprintf(program, sizeof program, "%scosmo%d", base, number);

    child_argv[0] = program;
    child_argv[1] = (argc > 2) ? argv[2] : NULL;  /* the game's write path */
    child_argv[2] = NULL;

    child = SDL_CreateProcess(child_argv, false);
    if (!child) {
        fprintf(stderr, "cosmo: cannot start %s (%s)\n", program, SDL_GetError());
        return 1;
    }

    {
        int status = 0;

        SDL_WaitProcess(child, true, &status);
        SDL_DestroyProcess(child);
        return status;
    }
}
