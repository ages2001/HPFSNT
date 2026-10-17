@echo off
REM pinball.sys (HPFSNT) for NT 3.1, 3.5, 3.51 and 4.0 (also loads on 2000
REM and XP). MSVCDIR = Visual C++ 4.x, DDKDIR = NT4 DDK.
REM The NT4 DDK has no ntifs.h: the free one (release 58, Bo Branten,
REM http://www.acc.umu.se/~bosse/) is in this folder.
REM libcntpr.lib comes before ntoskrnl.lib so the 64-bit arithmetic helpers
REM (_allmul, _aulldiv, ...) are linked in: NT 3.x does not export them.
REM
REM Subsystem version 3.10 and -base 0x10000 so the NT 3.1 loader accepts it.
REM HPFS_NT31 selects the universal profile: the executive-resource API is
REM bound to the running kernel at load time (hpfsres.c), so one binary loads
REM on NT 3.1 (legacy resources) through 4.0 (Lite). -release writes the PE
REM checksum the NT 3.51+ loader validates.
REM
REM The tested binary is the GCC cross build (build/build_cross.sh).

set MSVCDIR=C:\MSDEV
set DDKDIR=C:\NT4DDK

if not exist ntifs.h echo ntifs.h not found in this folder -- see above.
if not exist ntifs.h goto error

set PATH=%MSVCDIR%\BIN;%DDKDIR%\BIN;%PATH%
set INCLUDE=.;%DDKDIR%\inc;%DDKDIR%\inc\crt;%MSVCDIR%\INCLUDE
set LIB=%DDKDIR%\lib\i386;%MSVCDIR%\LIB;%DDKDIR%\lib\i386\free;%LIB%

if exist *.obj del *.obj
if exist pinball.sys del pinball.sys

cl -nologo -c -Gz -Ox -W3 -Zp8 -Zi -Zl -DHPFS_KERNEL -DHPFS_NT4 -DHPFS_NT31 -D_X86_=1 -Di386=1 -DSTD_CALL -DCONDITION_HANDLING=1 -DNT_UP=0 -DNT_INST=0 -DWIN32=100 -D_NT1X_=100 -DWINNT=1 -D_WIN32_WINNT=0x0400 -DDEVL=1 -DFPO=1 /I. /I.. ..\hpfsalc.c ..\hpfsclos.c ..\hpfscrea.c ..\hpfsdir.c ..\hpfsdisp.c ..\hpfsdnd.c ..\hpfsea.c ..\hpfsfast.c ..\hpfsfil.c ..\hpfsflsh.c ..\hpfsfsct.c ..\hpfsinfo.c ..\hpfsinit.c ..\hpfsio.c ..\hpfsmeta.c ..\hpfsmisc.c ..\hpfsread.c ..\hpfsres.c ..\hpfsseh.c ..\hpfsseti.c ..\hpfsstrc.c ..\hpfssup.c ..\hpfsvol.c ..\hpfswrit.c
if errorlevel 1 goto error

link -nologo -release -version:1.0 -debug -debugtype:both -subsystem:native,3.10 -entry:DriverEntry@8 -driver -base:0x10000 -align:0x200 -nodefaultlib -out:pinball.sys hpfsalc.obj hpfsclos.obj hpfscrea.obj hpfsdir.obj hpfsdisp.obj hpfsdnd.obj hpfsea.obj hpfsfast.obj hpfsfil.obj hpfsflsh.obj hpfsfsct.obj hpfsinfo.obj hpfsinit.obj hpfsio.obj hpfsmeta.obj hpfsmisc.obj hpfsread.obj hpfsres.obj hpfsseh.obj hpfsseti.obj hpfsstrc.obj hpfssup.obj hpfsvol.obj hpfswrit.obj libcntpr.lib ntoskrnl.lib hal.lib
if errorlevel 1 goto error

echo pinball.sys built.
goto end

:error
echo Build FAILED.

:end
