#!/bin/bash
# The C compiler for FFmpeg's console build (scripts/build-ffmpeg.sh): xsdk's GCC, with one instruction written
# around in what it compiles. Xenon runs lha, lhax and lhau (a 16-bit load that extends the sign) from microcode,
# 16 cycles slower than the same load without the sign followed by extsh (decbench, 2026-11-25), and GCC has no
# switch against them. Every compile goes to assembly first, has each such load rewritten into that pair, which
# gives the same register and touches no flag, and is then assembled. Anything else (linking, configure's probes
# that don't compile a .c to a .o) goes to GCC untouched.
real=/opt/xsdk-gcc/bin/powerpc-xsdk-elf-gcc
compile=0 src= out= prev=
for a in "$@"; do
  case "$a" in
    -c) compile=1 ;;
    *.c) src=$a ;;
  esac
  [ "$prev" = -o ] && out=$a
  prev=$a
done
if [ $compile = 0 ] || [ -z "$src" ] || [ "${out%.o}" = "$out" ]; then
  exec "$real" "$@"
fi
asm=${out%.o}.xenon.s
args=() machine=() prev=
for a in "$@"; do
  if [ "$prev" = -o ]; then args+=("$asm")
  elif [ "$a" = -c ]; then args+=(-S)
  else args+=("$a"); fi
  case "$a" in -m*) machine+=("$a") ;; esac
  prev=$a
done
"$real" "${args[@]}" || exit $?
sed -E -i \
  -e 's/^\tlha ([0-9]+),(.*)$/\tlhz \1,\2\n\textsh \1,\1/' \
  -e 's/^\tlhax ([0-9]+),(.*)$/\tlhzx \1,\2\n\textsh \1,\1/' \
  -e 's/^\tlhau ([0-9]+),(.*)$/\tlhzu \1,\2\n\textsh \1,\1/' \
  -e 's/^\tlhaux ([0-9]+),(.*)$/\tlhzux \1,\2\n\textsh \1,\1/' "$asm" || exit $?
# The pairs are four bytes longer than GCC counted, which can put a conditional branch's target out of its reach
# (32 KB) in a very long function. The assembler names each such line; it becomes the opposite branch around a plain
# one, and the file is assembled again.
for round in 1 2 3 4 5 6 7 8; do
  errors=$("$real" "${machine[@]}" -c "$asm" -o "$out" 2>&1) && { [ -n "$errors" ] && echo "$errors" >&2; rm -f "$asm"; exit 0; }
  lines=$(echo "$errors" | sed -nE 's/^[^:]+:([0-9]+): Error: operand out of range.*/\1/p' | sort -un)
  if [ -z "$lines" ] || [ "$(echo "$errors" | grep -c 'Error:')" != "$(echo "$lines" | wc -l)" ]; then
    echo "$errors" >&2
    exit 1
  fi
  script=()
  for n in $lines; do
    while read -r from to; do
      script+=(-e "${n}s/^\t${from}[+-]? (([0-9]+,)?)([^ ,]+)\$/\t${to} \1.+8\n\tb \3/")
    done <<'PAIRS'
beq bne
bne beq
blt bge
bge blt
bgt ble
ble bgt
bnl blt
bng bgt
bun bnu
bnu bun
bso bns
bns bso
bdnz bdz
bdz bdnz
PAIRS
  done
  before=$(cksum < "$asm")
  # Later lines first, so that a line's number still holds when the one before it has grown.
  sorted=()
  for n in $(echo "$lines" | sort -rn); do
    for e in "${script[@]}"; do case "$e" in "${n}s/"*) sorted+=(-e "$e") ;; esac; done
  done
  sed -E -i "${sorted[@]}" "$asm" || exit $?
  [ "$(cksum < "$asm")" = "$before" ] && { echo "$errors" >&2; echo "xenon-cc.sh: a branch it cannot rewrite" >&2; exit 1; }
done
echo "xenon-cc.sh: branches still out of range after 8 rounds" >&2
exit 1
