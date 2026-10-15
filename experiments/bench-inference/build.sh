#!/usr/bin/env bash
# Compila o bench.c linkando a libxgboost e, se existir, o C gerado pelo TL2cgen.
set -euo pipefail

OUT="${1:-build}"
CC="${CC:-gcc}"

# --- localiza a libxgboost dentro do pacote Python ---
XGB_DIR="$(python -c 'import xgboost, os; print(os.path.dirname(xgboost.__file__))')"
XGB_LIB=""
for cand in "$XGB_DIR/lib/libxgboost.so" "$XGB_DIR/libxgboost.so" \
            "$XGB_DIR/lib/libxgboost.dylib" "$XGB_DIR/libxgboost.dylib"; do
    [ -f "$cand" ] && XGB_LIB="$cand" && break
done
if [ -z "$XGB_LIB" ]; then
    echo "ERRO: libxgboost nao encontrada em $XGB_DIR" >&2
    exit 1
fi
XGB_LIBDIR="$(dirname "$XGB_LIB")"
echo "[build] libxgboost: $XGB_LIB"

# --- headers do xgboost, se o wheel os trouxer (opcional) ---
XGB_FLAGS=()
for inc in "$XGB_DIR/include" "$XGB_DIR/../include"; do
    if [ -f "$inc/xgboost/c_api.h" ]; then
        XGB_FLAGS=(-DUSE_XGB_HEADER -I"$inc")
        echo "[build] headers do xgboost: $inc"
        break
    fi
done
[ ${#XGB_FLAGS[@]} -eq 0 ] && echo "[build] sem headers; usando as declaracoes do bench.c"

# --- C gerado pelo TL2cgen, se prepare.py ja rodou ---
# O C gerado tem centenas de milhares de linhas e leva minutos para compilar,
# entao viramos objeto UMA vez e reaproveitamos enquanto o .c nao mudar.
TL_DIR="$OUT/tl2cgen_c"
TL_FLAGS=()
TL_OBJS=()
if [ -d "$TL_DIR" ] && compgen -G "$TL_DIR/*.c" > /dev/null; then
    TL_FLAGS=(-DHAVE_TL2CGEN -I"$TL_DIR")
    for src in "$TL_DIR"/*.c; do
        obj="${src%.c}.o"
        if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
            echo "[build] compilando $(basename "$src") (pode levar minutos)..."
            $CC -O2 -std=c99 -I"$TL_DIR" -c "$src" -o "$obj"
        else
            echo "[build] reaproveitando $(basename "$obj")"
        fi
        TL_OBJS+=("$obj")
    done
else
    echo "[build] TL2cgen nao gerado; o bench mede so a libxgboost"
fi

$CC -O2 -std=c99 -o "$OUT/bench" bench.c "${TL_OBJS[@]+"${TL_OBJS[@]}"}" \
    "${XGB_FLAGS[@]+"${XGB_FLAGS[@]}"}" "${TL_FLAGS[@]+"${TL_FLAGS[@]}"}" \
    -L"$XGB_LIBDIR" -lxgboost -Wl,-rpath,"$XGB_LIBDIR" -lm

echo "[build] ok -> $OUT/bench"
