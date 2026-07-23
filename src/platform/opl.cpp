/*
 * opl.cpp -- YM3812 synthesis, wrapping ymfm.
 *
 * The only C++ in the project, and deliberately thin: it exists so ymfm's
 * chip core can be reached from C. See include/cosmo/opl.h for why the timers
 * are not handled here.
 */

#include <mutex>

#include "ymfm_opl.h"

#include "cosmo/opl.h"

namespace {

/*
 * ymfm calls back into the host for timers, IRQs and busy signalling. None of
 * that applies here: the game reads timer state through the status register,
 * which ports.c answers from its own model of the two counters. Leaving these
 * as the defaults keeps the synthesiser free of any timing responsibility.
 */
class cosmo_ymfm_interface : public ymfm::ymfm_interface
{
};

cosmo_ymfm_interface interface_instance;
ymfm::ym3812 chip(interface_instance);

/*
 * Register writes come from the game thread by way of the timer interrupt,
 * while generation happens on the audio thread. ymfm has no internal locking,
 * so it needs one here.
 */
std::mutex chip_mutex;

}  // namespace

void opl_init(void)
{
    std::lock_guard<std::mutex> lock(chip_mutex);
    chip.reset();
}

void opl_reset(void)
{
    opl_init();
}

void opl_write(uint8_t reg, uint8_t value)
{
    std::lock_guard<std::mutex> lock(chip_mutex);

    chip.write_address(reg);
    chip.write_data(value);
}

void opl_generate(int16_t *out, int frames)
{
    std::lock_guard<std::mutex> lock(chip_mutex);

    for (int i = 0; i < frames; i++) {
        ymfm::ym3812::output_data sample;

        chip.generate(&sample, 1);

        /*
         * The YM3812 has a single output. Measured across a minute of the
         * game's own music it peaks around a third of full scale, so it is
         * lifted before clamping -- an AdLib was not a quiet card, and leaving
         * it raw puts the music well below the sound effects.
         */
        int32_t value = sample.data[0] * OPL_OUTPUT_GAIN;

        if (value > 32767) value = 32767;
        if (value < -32768) value = -32768;

        out[i] = static_cast<int16_t>(value);
    }
}
