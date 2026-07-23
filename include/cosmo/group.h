/*
 * group.h -- Reading the game's STN and VOL group files.
 *
 * A group file opens with a 960 byte header of 20 byte records: a 12 byte
 * name, then a 32-bit offset and length. The game's own reader is
 * GroupEntryFp() in game2.c; this is the same format, for the tools and the
 * launcher, which need to reach the data without linking the game.
 */

#ifndef COSMO_GROUP_H
#define COSMO_GROUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GROUP_HEADER_SIZE 960
#define GROUP_ENTRY_SIZE   20

/*
 * Read an entry into `dest`. `datadir` holds the group files and `episode`
 * picks between COSMO1, COSMO2 and COSMO3; the STN is tried before the VOL,
 * as the game does. Returns the number of bytes read, or 0 if not found.
 */
size_t group_read(const char *datadir, int episode, const char *entry_name,
                  void *dest, size_t max);

/* Whether both group files for an episode are present in `datadir`. */
bool group_episode_present(const char *datadir, int episode);

#endif /* COSMO_GROUP_H */
