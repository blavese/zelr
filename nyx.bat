@echo off
rem Boot nyx in QEMU. Double-click it, or run it with one of the words below.
rem
rem   nyx.bat              a window, on hardware like a machine from 2010
rem   nyx.bat modern       q35, UEFI, four processors, an NVMe disk
rem   nyx.bat iso          boot the disc image the way a real machine would
rem   nyx.bat serial       no window, everything on this console
rem   nyx.bat test         run the kernel's own checks and report
rem
rem The disk is nyx.img next to this file. It is made on first run and kept
rem afterwards, which is the point: files written in nyx are still there the
rem next time. Delete it to start clean.
rem
rem The memory given below is more than nyx will use. It maps 64 MiB of
rem itself and ignores the rest, so -m 512 and -m 4096 look identical from
rem inside; what makes it smooth is the acceleration, not the size.

setlocal enabledelayedexpansion
cd /d "%~dp0"

rem --- find qemu ------------------------------------------------------------
set "QEMU="
if exist "%ProgramFiles%\qemu\qemu-system-x86_64.exe" set "QEMU=%ProgramFiles%\qemu\qemu-system-x86_64.exe"
if not defined QEMU if exist "%ProgramFiles(x86)%\qemu\qemu-system-x86_64.exe" set "QEMU=%ProgramFiles(x86)%\qemu\qemu-system-x86_64.exe"
if not defined QEMU for %%I in (qemu-system-x86_64.exe) do if not "%%~$PATH:I"=="" set "QEMU=%%~$PATH:I"

if not defined QEMU (
  echo QEMU is not installed, or not where this looked.
  echo.
  echo   winget install SoftwareFreedomConservancy.QEMU
  echo.
  echo Or set QEMU to the full path of qemu-system-x86_64.exe and run again.
  pause
  exit /b 1
)

rem --- the kernel must have been built --------------------------------------
if not exist "build\nyx.bin" (
  echo build\nyx.bin is not there. Build it first:
  echo.
  echo   bash build.sh
  echo.
  pause
  exit /b 1
)

rem --- hardware acceleration -------------------------------------------------
rem
rem This is the whole difference between smooth and choppy. Without it QEMU
rem interprets every instruction in software, which is about five times slower
rem here: the kernel's own checks take 44 seconds that way and 9 with it.
rem
rem whpx is the Windows Hypervisor Platform. It needs that Windows feature
rem turned on, and it will not start when something else already owns the
rem processor's virtualisation, which usually means WSL2, Docker Desktop or
rem Hyper-V. Writing the two separated by a colon asks QEMU to try them in
rem order and say "falling back to tcg" if it has to, which is better than
rem probing for it here: a probe means running the machine once before
rem running it, and the honest probe took nine seconds.
set "ACCEL=accel=whpx:tcg"

rem --- the disk -------------------------------------------------------------
if not exist "nyx.img" (
  echo Making nyx.img, 64 MiB. Files written in nyx will be kept in it.
  fsutil file createnew nyx.img 67108864 >nul 2>&1
  if errorlevel 1 (
    rem fsutil needs a privilege some setups withhold; PowerShell does not.
    powershell -NoProfile -Command "$f=[IO.File]::Create('nyx.img');$f.SetLength(67108864);$f.Close()"
  )
)

set "MODE=%~1"
if "%MODE%"=="" set "MODE=plain"

if /i "%MODE%"=="modern" goto modern
if /i "%MODE%"=="iso"    goto iso
if /i "%MODE%"=="serial" goto serial
if /i "%MODE%"=="test"   goto test
if /i "%MODE%"=="plain"  goto plain

echo Do not know "%MODE%". Try: modern, iso, serial, test, or nothing at all.
pause
exit /b 1

rem --------------------------------------------------------------------------
:plain
rem An ordinary machine: one processor, a SATA disk, a network card. This is
rem the configuration most likely to just work.
echo Booting nyx. Close the window to stop it.
"%QEMU%" -machine q35,%ACCEL% -smp 2 -m 512 -no-reboot ^
  -drive "file=nyx.img,format=raw,if=none,id=d0" ^
  -device ahci,id=ahci -device ide-hd,drive=d0,bus=ahci.0 ^
  -netdev user,id=n0 -device e1000,netdev=n0 ^
  -kernel build\nyx.bin -serial stdio
goto done

rem --------------------------------------------------------------------------
:modern
rem What a laptop bought this decade looks like: UEFI firmware, four
rem processors, and an NVMe disk rather than anything resembling a cable.
rem The keyboard is still PS/2, because nyx has no USB stack yet and a
rem machine with only USB input would boot to a desktop you cannot type on.
set "FW=%ProgramFiles%\qemu\share\edk2-x86_64-code.fd"
if not exist "%FW%" (
  echo The UEFI firmware that ships with QEMU is not at:
  echo   %FW%
  echo Falling back to BIOS.
  goto plain
)
rem This one boots the disc rather than being handed the kernel. QEMU's
rem -kernel loads a kernel itself, which is a shortcut that skips the firmware
rem entirely; give it UEFI firmware as well and the firmware runs, finds
rem nothing it can boot, and drops you at an EFI shell. Booting the image goes
rem through nyx's own UEFI loader, which is the path a real machine takes.
if not exist "build\nyx.iso" (
  echo build\nyx.iso is not there, and UEFI needs it rather than the bare
  echo kernel. Make it with:
  echo.
  echo   python tools\mkiso.py
  echo.
  pause
  exit /b 1
)
echo Booting nyx on UEFI, four processors, NVMe. Close the window to stop it.
"%QEMU%" -machine q35,%ACCEL% -smp 4 -m 1024 -no-reboot ^
  -drive "if=pflash,format=raw,readonly=on,file=%FW%" ^
  -cdrom build\nyx.iso -boot d ^
  -drive "file=nyx.img,format=raw,if=none,id=nv0" ^
  -device nvme,drive=nv0,serial=nyx0001 ^
  -netdev user,id=n0 -device e1000,netdev=n0 ^
  -serial stdio
goto done

rem --------------------------------------------------------------------------
:iso
rem Through nyx's own bootloader rather than QEMU's -kernel shortcut, which
rem is the only way to exercise the path a real machine takes.
if not exist "build\nyx.iso" (
  echo build\nyx.iso is not there. Make it with:
  echo   python tools\mkiso.py
  pause
  exit /b 1
)
echo Booting the disc image. Close the window to stop it.
"%QEMU%" -machine q35,%ACCEL% -smp 2 -m 512 -no-reboot -cdrom build\nyx.iso -boot d ^
  -drive "file=nyx.img,format=raw,if=none,id=d0" ^
  -device ahci,id=ahci -device ide-hd,drive=d0,bus=ahci.0 ^
  -netdev user,id=n0 -device e1000,netdev=n0 -serial stdio
goto done

rem --------------------------------------------------------------------------
:serial
echo No window. Everything appears here; Ctrl+C stops it.
"%QEMU%" -machine q35,%ACCEL% -smp 2 -m 512 -no-reboot -display none ^
  -drive "file=nyx.img,format=raw,if=none,id=d0" ^
  -device ahci,id=ahci -device ide-hd,drive=d0,bus=ahci.0 ^
  -kernel build\nyx.bin -serial stdio
goto done

rem --------------------------------------------------------------------------
:test
echo Running the kernel's own checks.
"%QEMU%" -machine q35,%ACCEL% -smp 2 -m 512 -no-reboot -display none ^
  -drive "file=nyx.img,format=raw,if=none,id=d0" ^
  -device ahci,id=ahci -device ide-hd,drive=d0,bus=ahci.0 ^
  -device isa-debug-exit,iobase=0xf4,iosize=0x04 ^
  -kernel build\nyx.bin -append selftest -serial stdio
rem isa-debug-exit reports (code shifted left, or one), so 1 means it passed.
if errorlevel 2 (
  echo.
  echo Something failed. The lines above say what.
) else (
  echo.
  echo Everything passed.
)
goto done

:done
endlocal
if "%~1"=="" pause
