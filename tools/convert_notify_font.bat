@echo off
REM Regenerates notify.bin (BLE/ANCS notification CJK font).
REM
REM Usage:
REM   1) Drag a .ttf file onto this .bat  -> uses that font directly, or
REM   2) Double-click with no drag        -> if exactly one .ttf sits next
REM                                          to this .bat, uses it; if more
REM                                          than one, lists them and asks
REM                                          you to pick a number.
REM common_hanzi.txt must be in this same folder either way.
REM
REM Note: the installed lv_font_conv has no --symbols-file flag, only
REM --symbols <string> (inline characters, not a path) -- so the hanzi
REM list is read from common_hanzi.txt and passed through via PowerShell
REM instead of a plain cmd variable (cmd's ~8191-char variable limit is
REM too small for a multi-thousand-character list).

setlocal enabledelayedexpansion

set FONT_FILE=%~1

if not defined FONT_FILE (
    set COUNT=0
    for %%F in ("%~dp0*.ttf") do (
        set /a COUNT+=1
        set "TTF_!COUNT!=%%F"
    )

    if "!COUNT!"=="0" (
        echo No .ttf file found in this folder. Put a font file here, or drag one onto this .bat.
        pause
        exit /b 1
    )

    if "!COUNT!"=="1" (
        set "FONT_FILE=!TTF_1!"
    ) else (
        echo Multiple .ttf files found in this folder:
        echo.
        for /l %%i in (1,1,!COUNT!) do echo   %%i^) !TTF_%%i!
        echo.
        set /p PICK=Pick a number:
        REM Double indirection: %PICK% can't be used directly here since
        REM this whole if/else block is parsed (and its % variables
        REM expanded) once, before "set /p" ever actually runs -- so plain
        REM %PICK% would always be empty. "call set" forces a second,
        REM fresh parsing pass at execution time, after !PICK! (delayed
        REM expansion, resolved at run time) has already been substituted
        REM with the real number the user typed.
        call set "FONT_FILE=%%TTF_!PICK!%%"
    )
)

if not defined FONT_FILE (
    echo Invalid selection.
    pause
    exit /b 1
)

echo Using font: %FONT_FILE%
echo.

powershell -NoProfile -Command "$symbols = (Get-Content -Raw -Encoding UTF8 '%~dp0common_hanzi.txt') -replace '\s',''; & lv_font_conv --font '%FONT_FILE%' --size 24 --bpp 4 --symbols $symbols -r 0x20-0x7E -r 0x3000-0x303F -r 0xFF00-0xFFEF --format bin -o '%~dp0notify.bin'"

echo.
echo Done. Copy notify.bin to /sdcard/assets/fonts/bin/notify/notify.bin
pause
