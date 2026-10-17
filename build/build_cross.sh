#!/bin/sh
# HPFSNT cross build: produce a native NT 3.1 - 4.0 (also 2000/XP) pinball.sys
# with GCC + the mingw-w64 DDK headers, without Visual C++. Also builds the
# tools.
#
# Needs: gcc-mingw-w64-i686 (ships ddk headers), binutils, python3.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SRC="$ROOT/src"
OUT="${1:-/tmp/hpfsnt-cross}"
mkdir -p "$OUT"

GCC=i686-w64-mingw32-gcc
DLLTOOL=i686-w64-mingw32-dlltool
NM=i686-w64-mingw32-nm
INC="-I$SRC -I/usr/i686-w64-mingw32/include/ddk -I/usr/i686-w64-mingw32/include"
# -march=i486: NT 3.1 runs on 80486 (and 386) CPUs; no P6 instructions.
FLAGS="-c -ffreestanding -fno-stack-protector -fno-builtin -march=i486 -mtune=i486 \
 -D_X86_ -DWIN32 -D_WIN32_WINNT=0x0600 -DNTDDI_VERSION=0x06000000 -DHPFS_CROSS -w $EXTRACFLAGS"

echo "Compiling driver sources..."
rm -f "$OUT"/*.o
# The on-disk engine has no exception handling in it: optimise it
for f in hpfssup hpfsalc hpfsdnd hpfsfil; do
    $GCC $FLAGS -O2 -fno-strict-aliasing -DHPFS_KERNEL $INC "$SRC/$f.c" -o "$OUT/$f.o"
done
for f in "$SRC"/hpfs*.c; do
    b=$(basename "${f%.c}")
    case $b in hpfssup|hpfsalc|hpfsdnd|hpfsfil) continue;; esac
    $GCC $FLAGS $INC "$f" -o "$OUT/$b.o"
done
$GCC $FLAGS -O2 $INC "$HERE/hpfscrt.c" -o "$OUT/hpfscrt.o"

echo "Building the ntoskrnl import library from the symbols used..."
$NM "$OUT"/*.o 2>/dev/null | awk '/ U /{print $2}' | grep '^__imp_' \
    | sed 's/^__imp_//; s/^_//' | sort -u > "$OUT/need.txt"
{ echo "LIBRARY ntoskrnl.exe"; echo "EXPORTS"; cat "$OUT/need.txt"; } > "$OUT/ntoskrnl.def"
$DLLTOOL --kill-at -d "$OUT/ntoskrnl.def" -l "$OUT/libntoskrnl_nt31.a"

if [ -f "$NT31_EXPORTS" ]; then
    echo "Checking imports against NT 3.1 ntoskrnl..."
    sed 's/@[0-9]*$//' "$OUT/need.txt" | while read s; do
        grep -qx "$s" "$NT31_EXPORTS" || echo "  NOT EXPORTED BY NT 3.1: $s"
    done
fi

echo "Linking pinball.sys (native, subsystem 3.10, base 0x10000)..."
$GCC -shared -nostdlib -nostartfiles \
    -Wl,--subsystem,native -Wl,--major-subsystem-version,3 -Wl,--minor-subsystem-version,10 \
    -Wl,--major-os-version,1 -Wl,--minor-os-version,0 \
    -Wl,--major-image-version,1 -Wl,--minor-image-version,0 \
    -Wl,--image-base,0x10000 -Wl,--entry,_DriverEntry@8 -Wl,--file-alignment,0x200 \
    -o "$OUT/pinball.sys" "$OUT"/*.o "$OUT/libntoskrnl_nt31.a" -lgcc

i686-w64-mingw32-strip --strip-all "$OUT/pinball.sys"
python3 "$HERE/pechecksum.py" "$OUT/pinball.sys"

echo "Built: $OUT/pinball.sys"

# ----------------------------------------------------------------------
# The user-mode tools: freestanding console programs (subsystem 3.10) so
# they run on NT 3.1 with no C-runtime DLL; toolcrt.c is the tiny runtime.
# ----------------------------------------------------------------------
EXEBASE="-O2 -w -ffreestanding -nostdlib -nostartfiles -march=i486 -mtune=i486 -e _mainCRTStartup \
 -Wl,--subsystem,console -Wl,--major-subsystem-version,3 -Wl,--minor-subsystem-version,10 \
 -Wl,--major-os-version,1 -Wl,--minor-os-version,0 -D_WIN32 -D_CONSOLE"

TOOLCRT="-include $HERE/toolcrt.h \
 -D_INC_STDIO -D_INC_STDLIB -D_INC_STRING -D_INC_CTYPE -D_INC_TIME \
 -D_STDIO_H_=1 -D_STDLIB_H=1 -D_STRING_H_=1 -D_CTYPE_H_=1 -D_TIME_H_=1 \
 -Dmain=appmain -Dprintf=hp_printf -Dfprintf=hp_fprintf -Dsprintf=hp_sprintf -Dsnprintf=hp_snprintf -Dvsnprintf=hp_vsnprintf \
 -Dmalloc=hp_malloc -Dfree=hp_free -Dcalloc=hp_calloc -Drealloc=hp_realloc \
 -Dstrtoul=hp_strtoul -Dtime=hp_time -Dlocaltime=hp_localtime -Dtoupper=hp_toupper -D_stricmp=hp_stricmp"

echo "Linking hpfsea.exe..."
$GCC $EXEBASE $TOOLCRT -o "$OUT/hpfsea.exe" "$ROOT/tools/hpfsea.c" "$HERE/toolcrt.c" -lntdll -lkernel32 -lgcc

echo "Assembling the boot loader..."
gcc -E -P -x assembler-with-cpp "$ROOT/boot/hpfsboot.S" > "$OUT/hpfsboot.s"
as --32 -o "$OUT/hpfsboot.bo" "$OUT/hpfsboot.s"
ld -m elf_i386 -Ttext 0x7C00 --oformat binary -o "$OUT/hpfsboot.bin" "$OUT/hpfsboot.bo"
python3 "$HERE/bin2h.py" "$OUT/hpfsboot.bin" HpfsBootLoader > "$OUT/hpfsboot.h"

ENGINE="$SRC/hpfssup.c $SRC/hpfsalc.c $SRC/hpfsdnd.c $SRC/hpfsfil.c $ROOT/tools/hpfsimg.c"
TINC="-I$SRC -I$ROOT/tools -I$ROOT/fmt -I$ROOT/chk -I$OUT"

echo "Linking hpfsfmt.exe..."
$GCC $EXEBASE $TOOLCRT $TINC -o "$OUT/hpfsfmt.exe" "$ROOT/fmt/hpfsfmt.c" "$ROOT/fmt/hpfsfmtc.c" \
    $ENGINE "$HERE/toolcrt.c" -lkernel32 -lgcc

echo "Linking hpfschk.exe..."
$GCC $EXEBASE $TOOLCRT $TINC -o "$OUT/hpfschk.exe" "$ROOT/chk/hpfschk.c" "$ROOT/chk/hpfschkc.c" \
    $ENGINE "$HERE/toolcrt.c" -lkernel32 -lgcc

echo "Linking hpfsinst.exe..."
$GCC $EXEBASE $TOOLCRT -o "$OUT/hpfsinst.exe" "$ROOT/inst/hpfsinst.c" "$HERE/toolcrt.c" \
    -ladvapi32 -lkernel32 -lgcc

echo "Linking hpfsdasd.exe..."
$GCC $EXEBASE $TOOLCRT -I"$SRC" -o "$OUT/hpfsdasd.exe" "$ROOT/tools/hpfsdasd.c" "$HERE/toolcrt.c" -lkernel32 -lgcc

echo "Linking hpfsstrs.exe (test)..."
$GCC $EXEBASE $TOOLCRT -o "$OUT/hpfsstrs.exe" "$ROOT/test/hpfsstrs.c" "$HERE/toolcrt.c" -lkernel32 -lgcc

if [ -f "$NT31_USER_EXPORTS" ]; then
    echo "Checking the tools' imports against NT 3.1..."
    for f in "$OUT"/*.exe; do
        i686-w64-mingw32-objdump -p "$f" | awk '/^\t[0-9a-f]+\t +[0-9]+ +[A-Za-z_]/{print $3}' | while read s; do
            grep -qx "$s" "$NT31_USER_EXPORTS" || echo "  $(basename "$f"): $s is not in NT 3.1"
        done
    done
fi

echo "Done."
