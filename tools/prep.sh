#!/usr/bin/env bash
#
# Gera build/gen/ a partir de vendor/cosmore/src/ aplicando as transformacoes
# minimas necessarias para compilar fora do DOS. O submodulo permanece intocado,
# entao da pra auditar exatamente o que muda entre o codigo de 1992 e o nosso.
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC="$ROOT/vendor/cosmore/src"
GEN="$ROOT/build/gen"

if [ ! -d "$SRC" ]; then
    echo "erro: $SRC nao existe. Rode: git submodule update --init" >&2
    exit 1
fi

rm -rf "$GEN"
mkdir -p "$GEN"

cp "$SRC"/*.c "$SRC"/*.h "$GEN/"

# lowlevel.c: implementacao em C das rotinas de desenho, extraida do bloco de
# codigo de C-DRAWING.md do proprio upstream. E o que nos livra do TASM.
awk '/^```c$/{f=1;next} /^```$/{f=0} f' "$ROOT/vendor/cosmore/C-DRAWING.md" > "$GEN/lowlevel.c"

cd "$GEN"

for f in *.c *.h; do
    # 1. Headers Borland sem equivalente moderno -> substituidos por dos_compat.h
    sed -i '' -E '/^[[:space:]]*#[[:space:]]*include[[:space:]]*<(alloc|conio|dos|io|mem)\.h>/d' "$f"
    # 2. Assembly inline 16-bit -> comentado (reimplementado na plataforma).
    #    Comentario de linha, porque varias linhas asm ja terminam em /* ... */
    sed -i '' -E 's|^([[:space:]]*)asm[[:space:]]+(.*)$|\1// ASM: \2|' "$f"
done

echo "gerado em $GEN ($(ls "$GEN" | wc -l | tr -d ' ') arquivos)"
