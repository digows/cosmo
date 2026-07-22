/*
 * dos_compat.c -- Borland runtime and DOS memory services.
 *
 * The small, uninteresting half of the platform layer: string and memory
 * helpers Borland shipped that standard C never adopted, plus the pseudo
 * registers the compiler exposed as globals.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cosmo/dos_compat.h"

/*
 * Borland let C code read and write CPU registers through these. The game uses
 * them to pass arguments into BIOS calls, so they only need to be ordinary
 * storage that int86() and geninterrupt() agree on.
 */
unsigned int  _AX, _BX, _CX, _DX;
unsigned char _AH, _AL, _BH, _BL, _CH, _CL, _DH, _DL;

/*
 * Text mode video memory. The game writes one page of characters and
 * attributes here on the way out, which nothing renders yet, but the buffer
 * has to exist for the store to be harmless.
 */
static unsigned char text_mode_memory[4000];

void *dos_map_segment(unsigned seg, unsigned ofs)
{
    if (seg == 0xB800) {
        if (ofs >= sizeof text_mode_memory) return NULL;
        return text_mode_memory + ofs;
    }

    /*
     * Segment 0xA000 is video memory and must never be reached this way --
     * see the EGA_WRITE comment in dos_compat.h. Returning NULL turns a silent
     * corruption into an immediate, obvious crash.
     */
    return NULL;
}

void movmem(void *src, void *dest, unsigned length)
{
    if (!src || !dest) return;
    memmove(dest, src, length);
}

/*
 * DOS reported the largest free block. Nothing here is short of memory, and
 * the game only compares this against a threshold to decide whether it can
 * start, so report a comfortable figure well above what it asks for.
 */
unsigned long coreleft(void)
{
    return 655360UL;
}

long filelength(int handle)
{
    struct stat st;

    if (fstat(handle, &st) != 0) return -1L;
    return (long)st.st_size;
}

char *strupr(char *string)
{
    for (char *p = string; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
    return string;
}

char *ultoa(unsigned long value, char *string, int radix)
{
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char buffer[65];
    int i = 0;

    if (radix < 2 || radix > 36) {
        *string = '\0';
        return string;
    }

    do {
        buffer[i++] = digits[value % (unsigned long)radix];
        value /= (unsigned long)radix;
    } while (value);

    for (int j = 0; j < i; j++) {
        string[j] = buffer[i - 1 - j];
    }
    string[i] = '\0';

    return string;
}
