@echo off
REM hpfsea.exe and hpfsdasd.exe for NT 3.1 and later. MSVCDIR = Visual C++ 4.x.
REM hpfsea needs ntdll.lib (from the NT4 DDK: DDKDIR).
set MSVCDIR=C:\MSDEV
set DDKDIR=C:\NT4DDK
set PATH=%MSVCDIR%\BIN;%PATH%
set INCLUDE=%MSVCDIR%\INCLUDE
set LIB=%MSVCDIR%\LIB;%DDKDIR%\lib\i386\free
if exist *.obj del *.obj
cl -nologo -O2 -W3 -ML -D_WIN32 -D_CONSOLE ..\hpfsea.c ntdll.lib kernel32.lib -link -subsystem:console,3.10
if errorlevel 1 goto error
cl -nologo -O2 -W3 -ML -D_WIN32 -D_CONSOLE /I..\..\src ..\hpfsdasd.c kernel32.lib -link -subsystem:console,3.10
if errorlevel 1 goto error
echo hpfsea.exe and hpfsdasd.exe built.
goto end
:error
echo Build FAILED.
:end
