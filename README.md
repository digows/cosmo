# Cosmo — port nativo para macOS (Apple Silicon)

Port de **Cosmo's Cosmic Adventure: Forbidden Planet** (Apogee Software, 1992)
para rodar nativamente em macOS ARM64, sem emulador de DOS.

O ponto de partida é o código do jogo original, não uma reimplementação. Usamos
o [Cosmore](https://github.com/smitelli/cosmore) — a reconstrução do código-fonte
da v1.20 feita por Scott Smitelli a partir do desassembly dos executáveis, 96,3%
byte-accurate contra os binários de 1992 — e substituímos apenas a camada que
falava com o hardware do PC.

## Estado atual

| Subsistema | Estado |
|---|---|
| Compilação do código do jogo em ARM64 | ✅ 0 erros (14.600 linhas) |
| EGA emulado (write modes, latches, bit mask, map mask) | ✅ |
| Decodificação planar + paleta + apresentação SDL2 | ✅ validado |
| Leitura dos group files STN/VOL | ✅ (harness) |
| Camada de interrupções (int 8 / int 9) e temporização | ⬜ |
| Teclado e joystick | ⬜ |
| AdLib (OPL2) e PC speaker | ⬜ |
| Wiring do jogo completo | ⬜ |

O harness `firstframe` já renderiza as telas cheias do jogo a partir dos dados
originais, exercitando o caminho completo: group file → 4 planos EGA → paleta →
pixels.

## Como construir

Requer macOS com Xcode command line tools, `pkg-config` e SDL2.

```bash
brew install sdl2 pkg-config
```

```bash
git clone --recurse-submodules <este-repo> && cd cosmos && make
```

## Dados do jogo

Os assets são propriedade da Apogee Software e **não** estão neste repositório.
Coloque `COSMO1.STN` e `COSMO1.VOL` em `gamedata/`. Veja
[gamedata/README.md](gamedata/README.md).

Para ver o primeiro frame:

```bash
make firstframe ENTRY=TITLE1.MNI PNG=title.png
```

Sem `PNG=`, abre uma janela SDL com correção de proporção 4:3.

## Arquitetura

```
vendor/cosmore/     submódulo pinado no upstream (intocado)
tools/prep.sh       gera build/gen/ a partir do submódulo
include/            headers da camada de plataforma
src/platform/       EGA emulado, vídeo, compat DOS
tools/firstframe.c  harness de validação
```

O `prep.sh` aplica exatamente duas transformações no código do upstream: remove
os `#include` de headers Borland sem equivalente moderno, e comenta as linhas de
assembly inline 16-bit. Nada mais é editado — o diff contra o original fica
auditável.

As rotinas de desenho em assembly são substituídas pela implementação em C que o
próprio upstream publica em `C-DRAWING.md`. Isso elimina a dependência do Turbo
Assembler, que nunca foi liberado gratuitamente.

## Licença

Código deste repositório: MIT (veja [LICENSE](LICENSE)).

Cosmore: MIT, © Scott Smitelli e contribuidores.

*Cosmo's Cosmic Adventure*, seus assets e marcas: © 1992 Apogee Software, Ltd.
Veja [ATTRIBUTION.md](ATTRIBUTION.md).
