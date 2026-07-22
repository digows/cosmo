/*
 * ega.h -- EGA (IBM Enhanced Graphics Adapter) emulado em software.
 *
 * O Cosmo desenha falando direto com o hardware: programa o Sequencer e o
 * Graphics Controller por portas de I/O e escreve na VRAM em 0xA000. Aqui a
 * VRAM vira 4 planos de 64 KB em memoria comum, e toda leitura/escrita passa
 * por ega_read()/ega_write(), que reproduzem os write modes, os latches e o
 * bit mask do chip real.
 *
 * Enderecos sao lineares dentro da VRAM (0 = inicio de 0xA000), nao seg:ofs.
 */

#ifndef EGA_H
#define EGA_H

#include <stdint.h>

#define EGA_PLANES        4
#define EGA_PLANE_SIZE    0x10000u

/* Layout usado pelo jogo (ver graphics.h do Cosmore) */
#define EGA_PAGE_SIZE     0x2000u   /* distancia entre paginas de exibicao */
#define EGA_PAGE_BYTES    8000u     /* 320x200 / 8 pixels por byte, por plano */

#define EGA_SCREEN_W      320
#define EGA_SCREEN_H      200

/* Portas */
#define EGA_PORT_SEQ_INDEX  0x03C4
#define EGA_PORT_SEQ_DATA   0x03C5
#define EGA_PORT_GC_INDEX   0x03CE
#define EGA_PORT_GC_DATA    0x03CF

/* Indices do Graphics Controller */
#define GC_SET_RESET        0x00
#define GC_ENABLE_SET_RESET 0x01
#define GC_COLOR_COMPARE    0x02
#define GC_DATA_ROTATE      0x03
#define GC_READ_MAP         0x04
#define GC_MODE             0x05
#define GC_MISC             0x06
#define GC_COLOR_DONT_CARE  0x07
#define GC_BIT_MASK         0x08

/* Indices do Sequencer */
#define SEQ_MAP_MASK        0x02

typedef struct {
    uint8_t vram[EGA_PLANES][EGA_PLANE_SIZE];
    uint8_t latch[EGA_PLANES];

    /* Sequencer */
    uint8_t seq_index;
    uint8_t map_mask;         /* bits 0-3: planos habilitados para escrita */

    /* Graphics Controller */
    uint8_t gc_index;
    uint8_t set_reset;
    uint8_t enable_set_reset;
    uint8_t data_rotate;      /* bits 0-2 rotacao, bits 3-4 funcao logica */
    uint8_t read_map;
    uint8_t mode;             /* bits 0-1 write mode, bit 3 read mode */
    uint8_t bit_mask;

    /* Attribute Controller / CRTC (so o que o jogo usa) */
    uint8_t palette[16];      /* indice -> valor de cor EGA de 6 bits */
    uint16_t display_start;   /* offset linear da pagina exibida */
    uint8_t border_color;
} ega_state;

extern ega_state ega;

void    ega_init(void);
void    ega_reset_registers(void);

/* Escrita indexada: o jogo usa `outport(porta, (dado << 8) | indice)`, que no
 * 8086 manda o byte baixo pro index e o alto pro data. */
void    ega_out_word(uint16_t port, uint16_t value);
void    ega_out_byte(uint16_t port, uint8_t value);
uint8_t ega_in_byte(uint16_t port);

/* Acesso a VRAM. addr e linear a partir de 0xA000:0000. */
uint8_t ega_read(uint32_t addr);
void    ega_write(uint32_t addr, uint8_t value);

/* Conveniencia: converte um seg:ofs do codigo original em endereco linear. */
static inline uint32_t ega_addr(uint16_t seg, uint16_t ofs)
{
    return ((uint32_t)(seg - 0xA000u) << 4) + ofs;
}

/* Modo de video (int 10h AH=00h). So 0x0D importa aqui. */
void    ega_set_video_mode(uint8_t mode);
void    ega_set_palette_register(uint8_t index, uint8_t color_value);
void    ega_set_border_color(uint8_t color_value);
void    ega_select_active_page(uint8_t page);

#endif /* EGA_H */
