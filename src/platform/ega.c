/*
 * ega.c -- Implementacao do EGA emulado.
 *
 * Referencias: IBM PC Hardware Reference Library - Enhanced Graphics Adapter
 * (1984), e os comentarios do proprio lowlevel.asm do Cosmore.
 */

#include <string.h>

#include "ega.h"

ega_state ega;

/* Paleta default do modo 0Dh. O indice tem bit0=Azul, bit1=Verde, bit2=Vermelho
 * e bit3=Intensidade, batendo com o enum COLORS da Borland. */
static const uint8_t default_palette[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
};

void ega_reset_registers(void)
{
    ega.seq_index = 0;
    ega.map_mask = 0x0F;
    ega.gc_index = 0;
    ega.set_reset = 0x00;
    ega.enable_set_reset = 0x00;
    ega.data_rotate = 0x00;
    ega.read_map = 0x00;
    ega.mode = 0x00;
    ega.bit_mask = 0xFF;
    memset(ega.latch, 0, sizeof ega.latch);
}

void ega_init(void)
{
    memset(&ega, 0, sizeof ega);
    ega_reset_registers();
    memcpy(ega.palette, default_palette, sizeof default_palette);
    ega.display_start = 0;
    ega.border_color = 0;
}

/* ------------------------------------------------------------------------- */
/* Registradores                                                             */
/* ------------------------------------------------------------------------- */

static void seq_write(uint8_t index, uint8_t value)
{
    if (index == SEQ_MAP_MASK) {
        ega.map_mask = value & 0x0F;
    }
    /* Reset, Clocking Mode, Character Map e Memory Mode nao afetam o que o
     * Cosmo desenha; o jogo escreve neles so na inicializacao do modo. */
}

static void gc_write(uint8_t index, uint8_t value)
{
    switch (index) {
    case GC_SET_RESET:        ega.set_reset        = value & 0x0F; break;
    case GC_ENABLE_SET_RESET: ega.enable_set_reset = value & 0x0F; break;
    case GC_DATA_ROTATE:      ega.data_rotate      = value & 0x1F; break;
    case GC_READ_MAP:         ega.read_map         = value & 0x03; break;
    case GC_MODE:             ega.mode             = value;        break;
    case GC_BIT_MASK:         ega.bit_mask         = value;        break;
    default: break;
    }
}

void ega_out_byte(uint16_t port, uint8_t value)
{
    switch (port) {
    case EGA_PORT_SEQ_INDEX: ega.seq_index = value; break;
    case EGA_PORT_SEQ_DATA:  seq_write(ega.seq_index, value); break;
    case EGA_PORT_GC_INDEX:  ega.gc_index = value; break;
    case EGA_PORT_GC_DATA:   gc_write(ega.gc_index, value); break;
    default: break;
    }
}

/*
 * Um `out dx, ax` de 16 bits manda AL para a porta e AH para porta+1. O jogo
 * usa isso o tempo todo para escrever indice e dado numa tacada so.
 */
void ega_out_word(uint16_t port, uint16_t value)
{
    ega_out_byte(port, (uint8_t)(value & 0xFF));
    ega_out_byte(port + 1, (uint8_t)(value >> 8));
}

uint8_t ega_in_byte(uint16_t port)
{
    switch (port) {
    case EGA_PORT_SEQ_INDEX: return ega.seq_index;
    case EGA_PORT_GC_INDEX:  return ega.gc_index;
    default: return 0xFF;
    }
}

/* ------------------------------------------------------------------------- */
/* Acesso a VRAM                                                             */
/* ------------------------------------------------------------------------- */

/*
 * Toda leitura carrega os latches, mesmo que o valor retornado seja descartado.
 * O jogo depende disso: DrawSolidTile copia tile de VRAM para VRAM usando
 * `*dst = *src` com write mode 1, onde o dado da CPU e ignorado e o que vai
 * pra tela sao os latches carregados pela leitura.
 */
uint8_t ega_read(uint32_t addr)
{
    addr &= (EGA_PLANE_SIZE - 1);

    for (int p = 0; p < EGA_PLANES; p++) {
        ega.latch[p] = ega.vram[p][addr];
    }

    if (ega.mode & 0x08) {
        /* Read mode 1: color compare. O Cosmo nao usa, mas custa pouco. */
        uint8_t result = 0;
        uint8_t care = ega.set_reset;  /* Color Don't Care nao e rastreado */
        (void)care;
        for (int bit = 0; bit < 8; bit++) {
            uint8_t match = 1;
            for (int p = 0; p < EGA_PLANES; p++) {
                uint8_t planebit = (ega.latch[p] >> bit) & 1;
                uint8_t wanted = (ega.set_reset >> p) & 1;
                if (planebit != wanted) { match = 0; break; }
            }
            if (match) result |= (uint8_t)(1u << bit);
        }
        return result;
    }

    return ega.latch[ega.read_map];
}

static inline uint8_t rotate_right(uint8_t value, uint8_t count)
{
    count &= 7;
    if (count == 0) return value;
    return (uint8_t)((value >> count) | (value << (8 - count)));
}

static inline uint8_t apply_function(uint8_t data, uint8_t latch, uint8_t fn)
{
    switch (fn) {
    case 1: return (uint8_t)(data & latch);
    case 2: return (uint8_t)(data | latch);
    case 3: return (uint8_t)(data ^ latch);
    default: return data;  /* 0 = replace */
    }
}

void ega_write(uint32_t addr, uint8_t value)
{
    uint8_t write_mode = ega.mode & 0x03;
    uint8_t fn = (uint8_t)((ega.data_rotate >> 3) & 0x03);
    uint8_t rot = (uint8_t)(ega.data_rotate & 0x07);

    addr &= (EGA_PLANE_SIZE - 1);

    for (int p = 0; p < EGA_PLANES; p++) {
        uint8_t data;
        uint8_t result;

        if (!(ega.map_mask & (1u << p))) continue;

        switch (write_mode) {
        case 1:
            /* Latched write: o dado da CPU nao importa. */
            ega.vram[p][addr] = ega.latch[p];
            continue;

        case 2:
            /* Os 4 bits baixos do dado sao a cor; cada plano vira 0x00 ou 0xFF. */
            data = ((value >> p) & 1) ? 0xFF : 0x00;
            break;

        case 3:
            /* Nao usado pelo Cosmo (e nem existe no EGA original). */
            data = rotate_right(value, rot);
            break;

        default: /* write mode 0 */
            data = rotate_right(value, rot);
            if (ega.enable_set_reset & (1u << p)) {
                data = ((ega.set_reset >> p) & 1) ? 0xFF : 0x00;
            }
            break;
        }

        result = apply_function(data, ega.latch[p], fn);
        result = (uint8_t)((result & ega.bit_mask) |
                           (ega.latch[p] & (uint8_t)~ega.bit_mask));

        ega.vram[p][addr] = result;
    }
}

/* ------------------------------------------------------------------------- */
/* Servicos de BIOS (int 10h) que o jogo usa                                 */
/* ------------------------------------------------------------------------- */

void ega_set_video_mode(uint8_t mode)
{
    /* O unico modo relevante e 0Dh: 320x200, 16 cores, 8 paginas. */
    ega_reset_registers();
    memcpy(ega.palette, default_palette, sizeof default_palette);
    ega.display_start = 0;

    if (mode == 0x0D) {
        memset(ega.vram, 0, sizeof ega.vram);
    }
}

void ega_set_palette_register(uint8_t index, uint8_t color_value)
{
    if (index < 16) ega.palette[index] = color_value & 0x3F;
}

void ega_set_border_color(uint8_t color_value)
{
    ega.border_color = color_value & 0x3F;
}

void ega_select_active_page(uint8_t page)
{
    ega.display_start = (uint16_t)(page * EGA_PAGE_SIZE);
}
