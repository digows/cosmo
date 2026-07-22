/*
 * firstframe.c -- Harness de validacao da camada de video.
 *
 * NAO faz parte do port. Existe para provar, de ponta a ponta, que:
 *   1. sabemos ler os group files STN/VOL do jogo;
 *   2. o EGA emulado aplica map mask e write mode como o hardware real;
 *   3. o desentrelacamento dos 4 planos e a paleta produzem a imagem certa.
 *
 * O laco de escrita abaixo e uma copia fiel de DrawFullscreenImage()
 * (game1.c:558) do Cosmore -- mesma ordem de planos, mesmos valores de porta.
 * A unica diferenca e que as escritas vao para ega_write() em vez de um
 * ponteiro far para 0xA000.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "ega.h"
#include "video.h"

#define FULLSCREEN_IMAGE_SIZE 32000

/* Le uma entrada de group file. Mesma logica de GroupEntryFp() (game2.c:1472):
 * tenta o STN, depois o VOL. */
static long group_find(const char *path, const char *entry_name, long *length)
{
    unsigned char header[960];
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    if (fread(header, 1, sizeof header, fp) != sizeof header) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    for (size_t i = 0; i < sizeof header; i += 20) {
        if (header[i] == '\0') break;
        if (strncmp((const char *)header + i, entry_name, 11) == 0) {
            long off = (long)(header[i + 12] | (header[i + 13] << 8) |
                              (header[i + 14] << 16) | ((long)header[i + 15] << 24));
            *length  = (long)(header[i + 16] | (header[i + 17] << 8) |
                              (header[i + 18] << 16) | ((long)header[i + 19] << 24));
            return off;
        }
    }
    return -1;
}

static int load_entry(const char *datadir, const char *entry_name,
                      unsigned char *dest, size_t want)
{
    const char *groups[] = {"COSMO1.STN", "COSMO1.VOL"};

    for (int g = 0; g < 2; g++) {
        char path[512];
        long length = 0, off;

        snprintf(path, sizeof path, "%s/%s", datadir, groups[g]);
        off = group_find(path, entry_name, &length);
        if (off < 0) continue;

        FILE *fp = fopen(path, "rb");
        if (!fp) continue;
        if (fseek(fp, off, SEEK_SET) != 0) { fclose(fp); continue; }

        size_t n = fread(dest, 1, want, fp);
        fclose(fp);

        printf("  %s encontrado em %s (offset %ld, tamanho %ld, lidos %zu)\n",
               entry_name, groups[g], off, length, n);
        return n == want;
    }

    fprintf(stderr, "  %s nao encontrado em nenhum group file\n", entry_name);
    return 0;
}

int main(int argc, char *argv[])
{
    const char *datadir   = (argc > 1) ? argv[1] : "gamedata";
    const char *entry     = (argc > 2) ? argv[2] : "TITLE1.MNI";
    const char *png_path  = (argc > 3) ? argv[3] : NULL;
    unsigned char image[FULLSCREEN_IMAGE_SIZE];

    printf("Carregando %s de %s/\n", entry, datadir);
    if (!load_entry(datadir, entry, image, sizeof image)) return 1;

    ega_init();
    ega_set_video_mode(0x0D);

    /* EGA_MODE_DEFAULT() e EGA_BIT_MASK_DEFAULT(), de lowlevel.h */
    ega_out_word(0x03CE, (0x00 << 8) | 0x05);
    ega_out_word(0x03CE, (0xFF << 8) | 0x08);

    /* Laco de DrawFullscreenImage(): um plano por vez, 8000 bytes cada. */
    {
        unsigned mask = 0x0100;
        for (unsigned srcbase = 0; srcbase < 32000; srcbase += 8000) {
            ega_out_word(0x03C4, (uint16_t)(0x0002 | mask));
            for (unsigned i = 0; i < 8000; i++) {
                ega_write(i, image[i + srcbase]);
            }
            mask <<= 1;
        }
    }

    ega_select_active_page(0);

    if (png_path) {
        if (!video_write_png(png_path)) {
            fprintf(stderr, "falha ao gravar %s\n", png_path);
            return 1;
        }
        printf("Gravado %s (320x200)\n", png_path);
        return 0;
    }

    if (!video_init("Cosmo -- primeiro frame", 3)) return 1;
    while (video_pump_events()) {
        video_present();
        SDL_Delay(16);
    }
    video_shutdown();
    return 0;
}
