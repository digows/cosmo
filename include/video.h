/*
 * video.h -- Apresentacao: VRAM planar do EGA emulado -> tela.
 */

#ifndef VIDEO_H
#define VIDEO_H

#include <stdbool.h>
#include <stdint.h>

/* Converte a pagina exibida do EGA para RGB de 8 bits por canal.
 * `out` precisa ter EGA_SCREEN_W * EGA_SCREEN_H * 3 bytes. */
void video_render_rgb(uint8_t *out);

/* Grava a tela atual como PNG. Retorna false em erro de I/O. */
bool video_write_png(const char *path);

/* Janela SDL. `scale` e o fator horizontal; a altura e corrigida para 4:3. */
bool video_init(const char *title, int scale);
void video_present(void);
void video_shutdown(void);

/* Retorna false quando o usuario fecha a janela ou aperta ESC. */
bool video_pump_events(void);

#endif /* VIDEO_H */
