/*
 * input.h -- Keyboard translation and scripted input.
 */

#ifndef COSMO_INPUT_H
#define COSMO_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* An XT make code, or 0 when the key is not one the game knows. */
uint8_t input_xt_from_name(const char *name);

/*
 * Scripted input. COSMO_SCRIPT names a file of timed key events, which lets
 * the game be driven without a person at the keyboard -- for reproducing bugs
 * and for automated checks. Each line is:
 *
 *     <milliseconds> <down|up|tap> <key>
 *
 * Blank lines and lines starting with # are ignored. `tap` presses and
 * releases with a short gap, which is what a menu expects.
 */
void input_script_load(const char *path);

/*
 * Deliver any script events now due. `elapsed_ms` is measured from startup.
 * Returns the scancode to queue, or 0 when nothing is due; call repeatedly
 * until it returns 0.
 */
uint8_t input_script_next(uint32_t elapsed_ms);

/* True once every scripted event has been delivered. */
bool input_script_finished(void);

/* True when a script is in use at all. */
bool input_script_active(void);

#endif /* COSMO_INPUT_H */
