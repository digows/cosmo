/*
 * opl.h -- The AdLib's Yamaha YM3812 (OPL2), as a C interface.
 *
 * Synthesis is done by ymfm, which is C++; this is the boundary. Only the
 * sound generation lives behind here. The chip's two timers and its status
 * register are modelled in ports.c instead, because the game uses those for
 * detection and housekeeping rather than for making sound, and keeping them
 * out of the synthesiser keeps the audio thread away from that state.
 */

#ifndef COSMO_OPL_H
#define COSMO_OPL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The YM3812 in an AdLib ran from a 3.579545 MHz crystal and produced one
 * sample every 72 clocks, giving this rate. Generating at the chip's native
 * rate and letting SDL resample avoids writing a resampler and keeps the
 * synthesis exactly on the timing the register values assume.
 */
#define OPL_CLOCK_HZ  3579545u
#define OPL_SAMPLE_RATE 49716u

/*
 * Applied to the chip's output before clamping. See opl_generate().
 */
#define OPL_OUTPUT_GAIN 2

void opl_init(void);
void opl_reset(void);

/* A write to the chip's data port, with the address the game last selected. */
void opl_write(uint8_t reg, uint8_t value);

/* Render `frames` mono samples, replacing whatever is in `out`. */
void opl_generate(int16_t *out, int frames);

#ifdef __cplusplus
}
#endif

#endif /* COSMO_OPL_H */
