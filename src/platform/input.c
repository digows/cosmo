/*
 * input.c -- Key name translation and the scripted input driver.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cosmo/input.h"

#define MAX_SCRIPT_EVENTS 512
#define TAP_HOLD_MS 120

struct key_name {
    const char *name;
    uint8_t code;
};

/*
 * Only the keys Cosmo reads. The arrow keys deliberately map to the keypad
 * codes 0x48/0x4B/0x4D/0x50: with Num Lock off those are exactly what the
 * arrow cluster produced, and they are what the game ships as its defaults.
 */
static const struct key_name key_names[] = {
    {"esc", 0x01},    {"1", 0x02},      {"2", 0x03},      {"3", 0x04},
    {"4", 0x05},      {"5", 0x06},      {"6", 0x07},      {"7", 0x08},
    {"8", 0x09},      {"9", 0x0A},      {"0", 0x0B},      {"minus", 0x0C},
    {"equals", 0x0D}, {"backspace", 0x0E}, {"tab", 0x0F},
    {"q", 0x10}, {"w", 0x11}, {"e", 0x12}, {"r", 0x13}, {"t", 0x14},
    {"y", 0x15}, {"u", 0x16}, {"i", 0x17}, {"o", 0x18}, {"p", 0x19},
    {"enter", 0x1C}, {"ctrl", 0x1D},
    {"a", 0x1E}, {"s", 0x1F}, {"d", 0x20}, {"f", 0x21}, {"g", 0x22},
    {"h", 0x23}, {"j", 0x24}, {"k", 0x25}, {"l", 0x26},
    {"lshift", 0x2A},
    {"z", 0x2C}, {"x", 0x2D}, {"c", 0x2E}, {"v", 0x2F}, {"b", 0x30},
    {"n", 0x31}, {"m", 0x32},
    {"rshift", 0x36}, {"alt", 0x38}, {"space", 0x39},
    {"f1", 0x3B}, {"f2", 0x3C}, {"f3", 0x3D}, {"f4", 0x3E}, {"f5", 0x3F},
    {"f6", 0x40}, {"f7", 0x41}, {"f8", 0x42}, {"f9", 0x43}, {"f10", 0x44},
    {"home", 0x47}, {"up", 0x48},   {"pageup", 0x49},
    {"left", 0x4B}, {"right", 0x4D},
    {"end", 0x4F},  {"down", 0x50}, {"pagedown", 0x51},
    {"insert", 0x52}, {"delete", 0x53},
};

uint8_t input_xt_from_name(const char *name)
{
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++) {
        if (strcmp(key_names[i].name, name) == 0) return key_names[i].code;
    }
    return 0;
}

struct script_event {
    uint32_t at_ms;
    uint8_t scancode;   /* already carries bit 7 for a release */
};

static struct script_event script[MAX_SCRIPT_EVENTS];
static int script_count;
static int script_next_index;
static bool script_loaded;

static int compare_events(const void *a, const void *b)
{
    const struct script_event *ea = a, *eb = b;

    if (ea->at_ms < eb->at_ms) return -1;
    if (ea->at_ms > eb->at_ms) return 1;
    return 0;
}

static void add_event(uint32_t at_ms, uint8_t scancode)
{
    if (script_count >= MAX_SCRIPT_EVENTS) return;

    script[script_count].at_ms = at_ms;
    script[script_count].scancode = scancode;
    script_count++;
}

void input_script_load(const char *path)
{
    char line[256];
    FILE *fp;
    int line_number = 0;

    if (!path) return;

    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "cosmo: cannot open input script %s\n", path);
        return;
    }

    while (fgets(line, sizeof line, fp)) {
        unsigned long at_ms;
        char action[32], key[32];
        uint8_t code;

        line_number++;

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        if (sscanf(line, "%lu %31s %31s", &at_ms, action, key) != 3) {
            fprintf(stderr, "cosmo: %s:%d: cannot parse\n", path, line_number);
            continue;
        }

        code = input_xt_from_name(key);
        if (!code) {
            fprintf(stderr, "cosmo: %s:%d: unknown key '%s'\n",
                    path, line_number, key);
            continue;
        }

        if (strcmp(action, "down") == 0) {
            add_event((uint32_t)at_ms, code);
        } else if (strcmp(action, "up") == 0) {
            add_event((uint32_t)at_ms, (uint8_t)(code | 0x80));
        } else if (strcmp(action, "tap") == 0) {
            add_event((uint32_t)at_ms, code);
            add_event((uint32_t)at_ms + TAP_HOLD_MS, (uint8_t)(code | 0x80));
        } else {
            fprintf(stderr, "cosmo: %s:%d: unknown action '%s'\n",
                    path, line_number, action);
        }
    }

    fclose(fp);

    qsort(script, (size_t)script_count, sizeof script[0], compare_events);
    script_loaded = true;

    printf("cosmo: loaded %d scripted key events from %s\n",
           script_count, path);
    fflush(stdout);
}

uint8_t input_script_next(uint32_t elapsed_ms)
{
    if (script_next_index >= script_count) return 0;
    if (script[script_next_index].at_ms > elapsed_ms) return 0;

    return script[script_next_index++].scancode;
}

bool input_script_finished(void)
{
    return script_loaded && script_next_index >= script_count;
}

bool input_script_active(void)
{
    return script_loaded;
}
