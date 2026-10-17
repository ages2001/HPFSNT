#!/bin/sh
# HPFSNT for Windows XP x64 / Server 2003 x64 (amd64), with GCC
# (gcc-mingw-w64-x86-64): pinball.sys and the tools.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SRC="$ROOT/src"
OUT="${1:-/tmp/hpfsnt-amd64}"
mkdir -p "$OUT"

GCC=x86_64-w64-mingw32-gcc
INC="-I$SRC -I/usr/x86_64-w64-mingw32/include/ddk -I/usr/x86_64-w64-mingw32/include"
# The mingw DDK headers want the Vista level to be self-consistent; only
# functions XP x64 exports are imported (checked below).
FLAGS="-c -ffreestanding -fno-stack-protector -fno-builtin -mno-stack-arg-probe -fno-strict-aliasing \
 -D_AMD64_ -DWIN32 -D_WIN64 -D_WIN32_WINNT=0x0600 -DNTDDI_VERSION=0x06000000 -DHPFS_KERNEL -w $EXTRACFLAGS"

echo "Compiling driver sources (amd64)..."
rm -f "$OUT"/*.o
for f in "$SRC"/hpfs*.c; do
    b=$(basename "${f%.c}")
    case $b in hpfssup|hpfsalc|hpfsdnd|hpfsfil) O=-O2;; *) O=-O1;; esac
    $GCC $FLAGS $O $INC "$f" -o "$OUT/$b.o"
done
$GCC $FLAGS -O2 $INC "$HERE/hpfscrt.c" -o "$OUT/hpfscrt.o"

echo "Linking pinball.sys (native, subsystem 5.02)..."
$GCC -shared -nostdlib -nostartfiles \
    -Wl,--subsystem,native -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,2 \
    -Wl,--major-image-version,1 -Wl,--minor-image-version,0 \
    -Wl,--major-os-version,5 -Wl,--minor-os-version,2 \
    -Wl,--image-base,0x10000 -Wl,--entry,DriverEntry -Wl,--file-alignment,0x200 \
    -Wl,--section-alignment,0x1000 \
    -o "$OUT/pinball.sys" "$OUT"/*.o -lntoskrnl -lhal -lgcc
x86_64-w64-mingw32-strip --strip-all "$OUT/pinball.sys"
python3 "$HERE/pechecksum.py" "$OUT/pinball.sys"

if [ -f "$XP64_EXPORTS" ]; then
    echo "Checking imports against XP x64 ntoskrnl..."
    x86_64-w64-mingw32-objdump -p "$OUT/pinball.sys" | awk '/^\t[0-9a-f]+\t +[0-9]+ +[A-Za-z_]/{print $3}' | while read s; do
        grep -qx "$s" "$XP64_EXPORTS" || echo "  NOT EXPORTED BY XP x64: $s"
    done
fi
echo "Built: $OUT/pinball.sys"

# ----------------------------------------------------------------------
# Tools (console, no C runtime DLL)
# ----------------------------------------------------------------------
EXEBASE="-O2 -w -ffreestanding -nostdlib -nostartfiles -mno-stack-arg-probe -e mainCRTStartup \
 -Wl,--subsystem,console -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,2 \
 -Wl,--major-os-version,5 -Wl,--minor-os-version,2 -D_WIN32 -D_CONSOLE"
TOOLCRT="-include $HERE/toolcrt.h \
 -D_INC_STDIO -D_INC_STDLIB -D_INC_STRING -D_INC_CTYPE -D_INC_TIME \
 -D_STDIO_H_=1 -D_STDLIB_H=1 -D_STRING_H_=1 -D_CTYPE_H_=1 -D_TIME_H_=1 \
 -Dmain=appmain -Dprintf=hp_printf -Dfprintf=hp_fprintf -Dsprintf=hp_sprintf -Dsnprintf=hp_snprintf -Dvsnprintf=hp_vsnprintf \
 -Dmalloc=hp_malloc -Dfree=hp_free -Dcalloc=hp_calloc -Drealloc=hp_realloc \
 -Dstrtoul=hp_strtoul -Dtime=hp_time -Dlocaltime=hp_localtime -Dtoupper=hp_toupper -D_stricmp=hp_stricmp"

ENGINE="$SRC/hpfssup.c $SRC/hpfsalc.c $SRC/hpfsdnd.c $SRC/hpfsfil.c $ROOT/tools/hpfsimg.c"
TINC="-I$SRC -I$ROOT/tools -I$ROOT/fmt -I$ROOT/chk -I$ROOT/boot"

for t in fmt:hpfsfmt chk:hpfschk; do
    d=${t%%:*}; n=${t##*:}
    echo "Linking $n.exe..."
    $GCC $EXEBASE $TOOLCRT $TINC -o "$OUT/$n.exe" "$ROOT/$d/$n.c" "$ROOT/$d/${n}c.c" $ENGINE "$HERE/toolcrt.c" -lkernel32 -lgcc
done
echo "Linking hpfsinst.exe..."
$GCC $EXEBASE $TOOLCRT -o "$OUT/hpfsinst.exe" "$ROOT/inst/hpfsinst.c" "$HERE/toolcrt.c" -ladvapi32 -lkernel32 -lgcc
echo "Linking hpfsea.exe..."
$GCC $EXEBASE $TOOLCRT -o "$OUT/hpfsea.exe" "$ROOT/tools/hpfsea.c" "$HERE/toolcrt.c" -lntdll -lkernel32 -lgcc
echo "Linking hpfsdasd.exe..."
$GCC $EXEBASE $TOOLCRT -I"$SRC" -o "$OUT/hpfsdasd.exe" "$ROOT/tools/hpfsdasd.c" "$HERE/toolcrt.c" -lkernel32 -lgcc
echo "Linking hpfsstrs.exe (test)..."
$GCC $EXEBASE $TOOLCRT -o "$OUT/hpfsstrs.exe" "$ROOT/test/hpfsstrs.c" "$HERE/toolcrt.c" -lkernel32 -lgcc

if [ -f "$XP64_USER_EXPORTS" ]; then
    echo "Checking the tools' imports against XP x64..."
    for f in "$OUT"/*.exe; do
        x86_64-w64-mingw32-objdump -p "$f" | awk '/^\t[0-9a-f]+\t +[0-9]+ +[A-Za-z_]/{print $3}' | while read s; do
            grep -qx "$s" "$XP64_USER_EXPORTS" || echo "  $(basename "$f"): $s is not in XP x64"
        done
    done
fi
echo "Done."
