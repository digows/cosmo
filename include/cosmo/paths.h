/*
 * Where the game's files live.
 *
 * The 1992 code opens everything by bare filename, which on DOS meant the
 * directory the program was started from. That is not where a double-clicked
 * application starts, and inside a macOS bundle it is not somewhere anything
 * may be written, so the location has to be worked out rather than assumed.
 */

#ifndef COSMO_PATHS_H
#define COSMO_PATHS_H

#include <stdbool.h>

/*
 * Work out where the data is, make it reachable, and change into it.
 *
 * Two arrangements are supported. If the game data sits beside the executable
 * in a directory that can be written to -- an unzipped folder -- it is used
 * where it is, so a copy carried on a USB stick keeps its own saves. If it
 * cannot be written to, which is the case inside an application bundle, the
 * data is copied once into the user's own directory and used from there.
 *
 * Either way the working directory afterwards holds the group files and takes
 * the saves, so every relative path in the game means what it did in 1992.
 *
 * Returns false only if no game data could be found anywhere.
 */
bool paths_init(void);

/* The directory holding the group files. Valid after paths_init(). */
const char *paths_data_dir(void);

/*
 * Where saves and the configuration file belong. The same as the data
 * directory when the data is used in place.
 */
const char *paths_write_dir(void);

/*
 * Copy an episode's two group files into the data directory, given the path of
 * either one of them. Used when someone points the launcher at a copy of an
 * episode this port does not ship.
 */
bool paths_import_episode(const char *chosen_file, int episode);

#endif  /* COSMO_PATHS_H */
