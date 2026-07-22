/*
 * test_ega.c -- Unit tests for the emulated EGA.
 *
 * These run without any game data, so CI can catch a regression in the drawing
 * path on every platform. Each case covers a behaviour the game actually
 * depends on; a plausible-looking EGA that gets any of these wrong renders
 * garbage in ways that are painful to debug later.
 */

#include <stdio.h>
#include <string.h>

#include "cosmo/ega.h"

static int failures;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            printf("FAIL %s:%d: ", __func__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
            failures++;                                         \
        }                                                       \
    } while (0)

/* Writing a word to an index port must split into index and data. */
static void test_indexed_port_write(void)
{
    ega_init();

    ega_out_word(EGA_PORT_SEQ_INDEX, (0x0A << 8) | SEQ_MAP_MASK);
    CHECK(ega.map_mask == 0x0A, "map_mask is 0x%02X, expected 0x0A", ega.map_mask);

    ega_out_word(EGA_PORT_GC_INDEX, (0x3C << 8) | GC_BIT_MASK);
    CHECK(ega.bit_mask == 0x3C, "bit_mask is 0x%02X, expected 0x3C", ega.bit_mask);
}

/* The map mask must gate which planes a write reaches. */
static void test_map_mask_gates_planes(void)
{
    ega_init();

    ega_out_word(EGA_PORT_SEQ_INDEX, (0x05 << 8) | SEQ_MAP_MASK);  /* planes 0, 2 */
    ega_write(100, 0xFF);

    CHECK(ega.vram[0][100] == 0xFF, "plane 0 not written");
    CHECK(ega.vram[1][100] == 0x00, "plane 1 written but masked out");
    CHECK(ega.vram[2][100] == 0xFF, "plane 2 not written");
    CHECK(ega.vram[3][100] == 0x00, "plane 3 written but masked out");
}

/* The bit mask must preserve latch content in the bits it clears. */
static void test_bit_mask_merges_with_latches(void)
{
    ega_init();

    for (int p = 0; p < EGA_PLANES; p++) ega.vram[p][50] = 0xAA;
    (void)ega_read(50);  /* loads 0xAA into every latch */

    ega_out_word(EGA_PORT_GC_INDEX, (0x0F << 8) | GC_BIT_MASK);
    ega_write(50, 0xFF);

    /* High nibble keeps the latch (0xA), low nibble takes the new data (0xF). */
    CHECK(ega.vram[0][50] == 0xAF, "got 0x%02X, expected 0xAF", ega.vram[0][50]);
}

/*
 * Write mode 1 copies the latches and ignores the CPU data. DrawSolidTile
 * blits tiles from video memory to video memory this way, so getting it wrong
 * breaks every piece of scenery in the game.
 */
static void test_write_mode_1_copies_latches(void)
{
    ega_init();

    ega.vram[0][10] = 0x12;
    ega.vram[1][10] = 0x34;
    ega.vram[2][10] = 0x56;
    ega.vram[3][10] = 0x78;

    (void)ega_read(10);

    ega_out_word(EGA_PORT_GC_INDEX, (0x01 << 8) | GC_MODE);  /* write mode 1 */
    ega_write(20, 0x00);  /* data deliberately unrelated to the expected result */

    CHECK(ega.vram[0][20] == 0x12, "plane 0 got 0x%02X", ega.vram[0][20]);
    CHECK(ega.vram[1][20] == 0x34, "plane 1 got 0x%02X", ega.vram[1][20]);
    CHECK(ega.vram[2][20] == 0x56, "plane 2 got 0x%02X", ega.vram[2][20]);
    CHECK(ega.vram[3][20] == 0x78, "plane 3 got 0x%02X", ega.vram[3][20]);
}

/* Write mode 2 spreads the low four bits of the data across the planes. */
static void test_write_mode_2_expands_color(void)
{
    ega_init();

    ega_out_word(EGA_PORT_GC_INDEX, (0x02 << 8) | GC_MODE);
    ega_write(30, 0x09);  /* planes 0 and 3 set */

    CHECK(ega.vram[0][30] == 0xFF, "plane 0 got 0x%02X", ega.vram[0][30]);
    CHECK(ega.vram[1][30] == 0x00, "plane 1 got 0x%02X", ega.vram[1][30]);
    CHECK(ega.vram[2][30] == 0x00, "plane 2 got 0x%02X", ega.vram[2][30]);
    CHECK(ega.vram[3][30] == 0xFF, "plane 3 got 0x%02X", ega.vram[3][30]);
}

/* Set/reset substitutes a fixed color for the CPU data, per enabled plane. */
static void test_set_reset(void)
{
    ega_init();

    ega_out_word(EGA_PORT_GC_INDEX, (0x03 << 8) | GC_ENABLE_SET_RESET);
    ega_out_word(EGA_PORT_GC_INDEX, (0x01 << 8) | GC_SET_RESET);
    ega_write(40, 0x00);

    CHECK(ega.vram[0][40] == 0xFF, "plane 0 got 0x%02X", ega.vram[0][40]);
    CHECK(ega.vram[1][40] == 0x00, "plane 1 got 0x%02X", ega.vram[1][40]);
    /* Planes 2 and 3 have set/reset disabled, so they take the CPU data. */
    CHECK(ega.vram[2][40] == 0x00, "plane 2 got 0x%02X", ega.vram[2][40]);
}

/* Reads must load every latch, whichever plane the read map selects. */
static void test_read_loads_all_latches(void)
{
    uint8_t value;

    ega_init();

    for (int p = 0; p < EGA_PLANES; p++) ega.vram[p][60] = (uint8_t)(0x10 + p);

    ega_out_word(EGA_PORT_GC_INDEX, (0x02 << 8) | GC_READ_MAP);
    value = ega_read(60);

    CHECK(value == 0x12, "read returned 0x%02X, expected 0x12", value);
    for (int p = 0; p < EGA_PLANES; p++) {
        CHECK(ega.latch[p] == 0x10 + p, "latch %d is 0x%02X", p, ega.latch[p]);
    }
}

/* Page selection must move the displayed window, not the drawing target. */
static void test_page_selection(void)
{
    ega_init();

    ega_select_active_page(1);
    CHECK(ega.display_start == EGA_PAGE_SIZE,
          "display_start is 0x%04X", ega.display_start);

    ega_select_active_page(0);
    CHECK(ega.display_start == 0, "display_start is 0x%04X", ega.display_start);
}

int main(void)
{
    test_indexed_port_write();
    test_map_mask_gates_planes();
    test_bit_mask_merges_with_latches();
    test_write_mode_1_copies_latches();
    test_write_mode_2_expands_color();
    test_set_reset();
    test_read_loads_all_latches();
    test_page_selection();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("all EGA checks passed\n");
    return 0;
}
