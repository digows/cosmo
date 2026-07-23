/*
 * launcher.c -- Front door for the three episodes.
 *
 * Cosmo shipped as three programs and this port builds three too, which is not
 * a packaging decision: the episodes differ by preprocessor conditionals that
 * include or exclude whole actor implementations, so they compile to genuinely
 * different code. See the comment in CMakeLists.txt.
 *
 * The menu is drawn with the game's own font on the game's own title art,
 * through the same emulated EGA the game uses, so it looks like part of the
 * game rather than part of the host. Once an episode is chosen the process is
 * replaced by it -- not spawned as a child, which leaves two programs fighting
 * over one window.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#   include <process.h>
#   define EPISODE_PROGRAM_SUFFIX ".exe"
#else
#   include <unistd.h>
#   define EPISODE_PROGRAM_SUFFIX ""
#endif

#include <SDL3/SDL.h>

#include "cosmo/ega.h"
#include "cosmo/font.h"
#include "cosmo/group.h"
#include "cosmo/paths.h"
#include "cosmo/video.h"

#ifdef COSMO_HAVE_EPISODE4
#   define EPISODE_COUNT 4
#else
#   define EPISODE_COUNT 3
#endif
#define FULLSCREEN_IMAGE_SIZE 32000

/*
 * Where the menu sits, in font tiles. Kept low so it does not
 * cover the logo it is sitting on. The screen is 25 rows, so this reaches the
 * bottom exactly.
 */
#define MENU_TOP     15
#define MENU_HEIGHT  10
#define MENU_LEFT     3
#define MENU_WIDTH   34

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

static void draw_menu(int selected)
{
    char line[64];
    int row = MENU_TOP + 3;

    font_fill(MENU_LEFT, MENU_TOP, MENU_WIDTH, MENU_HEIGHT);

    font_draw_centered(MENU_TOP + 1, "Select an episode");

    /*
     * Every episode is listed, including the ones with no data. Hiding those
     * would hide the only route to installing them, and an episode marked as
     * missing is more use than an episode that is simply absent.
     */
    for (int i = 0; i < EPISODE_COUNT; i++) {
        /*
         * 0x1C is the right arrow in this font. The game's own keyNames[]
         * table uses it for the right cursor key, along with 0x18, 0x19 and
         * 0x1B for the other three -- not the CP437 assignments, but this is
         * the game's font and those are the glyphs in it.
         */
        snprintf(line, sizeof line, "%c %d) %s%s",
                 (i == selected) ? '\x1C' : ' ',
                 episodes[i].number, episodes[i].title,
                 episodes[i].present ? "" : " ...");
        font_draw(MENU_LEFT + 4, row, line);
        row++;
    }

    font_draw_centered(MENU_TOP + MENU_HEIGHT - 2,
                       "... not installed - pick to locate");
    font_draw_centered(MENU_TOP + MENU_HEIGHT - 1, "Enter to play, Esc to quit");
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
    return (selected + direction + EPISODE_COUNT) % EPISODE_COUNT;
}

/* ------------------------------------------------------------------------ */
/* Installing an episode this port does not ship                             */
/* ------------------------------------------------------------------------ */

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
        while (SDL_PollEvent(&event)) {
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

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) return 0;

            if (event.type != SDL_EVENT_KEY_DOWN) continue;

            switch (event.key.key) {
            case SDLK_ESCAPE:
            case SDLK_Q:
                return 0;

            case SDLK_UP:
            case SDLK_LEFT:
                selected = step_selection(selected, -1);
                redraw = true;
                break;

            case SDLK_DOWN:
            case SDLK_RIGHT:
                selected = step_selection(selected, 1);
                redraw = true;
                break;

            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE: {
                int chosen = choose_episode(selected);
                if (chosen) return chosen;
                redraw = true;
                break;
            }

            case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4: {
                int wanted = (int)(event.key.key - SDLK_1);
                if (wanted < EPISODE_COUNT) {
                    int chosen = choose_episode(wanted);
                    if (chosen) return chosen;
                    redraw = true;
                }
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
static int replace_process(const char *program, char *argv[])
{
#ifdef _WIN32
    return (int)_execv(program, (const char *const *)argv);
#else
    return execv(program, argv);
#endif
}

static int run_episode(int number)
{
    const char *base = SDL_GetBasePath();
    char program[1024];
    char *child_argv[2];

    if (!base) {
        fprintf(stderr, "cosmo: cannot locate the episode programs (%s)\n",
                SDL_GetError());
        return 1;
    }

    snprintf(program, sizeof program, "%scosmo%d%s",
             base, number, EPISODE_PROGRAM_SUFFIX);

    /* Give up the window before the process image is replaced. */
    video_shutdown();

    /*
     * No write path is handed over. paths_init() has already made the working
     * directory both the source of the group files and somewhere the saves can
     * go, so the game's own relative paths mean the right thing -- which also
     * avoids JoinPath(), whose 80-byte static buffer joins with a DOS
     * separator and never terminates its result.
     */
    child_argv[0] = program;
    child_argv[1] = NULL;

    replace_process(program, child_argv);

    /* Only reached if the replacement failed. */
    fprintf(stderr, "cosmo: cannot start %s\n", program);
    return 1;
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

    /*
     * The menu is shown even with a single episode installed. It is the only
     * place that explains how to add the two this port cannot ship, so
     * skipping it would hide them entirely.
     */
    (void)found;

    if (number == 0) {
        int background = episodes[first_present()].number;

        ega_init();
        ega_set_video_mode(0x0D);

        if (!video_init("Cosmo's Cosmic Adventure", 3)) return 1;

        if (!font_load(paths_data_dir(), background) || !draw_background(background)) {
            fprintf(stderr, "cosmo: episode data is present but unreadable\n");
            video_shutdown();
            return 1;
        }

        install_palette();
        ega_select_active_page(0);

        number = run_menu();

        if (number == 0) {
            video_shutdown();
            return 0;
        }
    }

    return run_episode(number);
}
