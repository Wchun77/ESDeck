@echo off
REM Force the next idf.py build to regenerate the compile date/time embedded
REM in esp_app_desc_t (the "Built ..." string shown on the Info screen).
REM
REM That timestamp is only written when esp_app_format's esp_app_desc.c gets
REM recompiled. Editing main.c or other project files does not make ninja
REM consider esp_app_desc.c dirty, so even after a full relink the embedded
REM date can stay stale. Deleting the cached object file forces the next
REM build to recompile just that one small file and relink - negligible
REM extra build time, but the date will be accurate again.
REM
REM Lives under tools\, uses a path relative to this script's own location
REM so it works regardless of where the project is checked out.

setlocal

set "TARGET=%~dp0..\build\esp-idf\esp_app_format\CMakeFiles\__idf_esp_app_format.dir\esp_app_desc.c.obj"

if not exist "%TARGET%" (
    echo [skip] not found: %TARGET%
    echo        project may not have been built yet, or build layout differs.
    goto :end
)

del /f /q "%TARGET%"
if errorlevel 1 (
    echo [error] delete failed - is the file locked by a running build/IDE?
) else (
    echo [ok] cleared esp_app_desc.c.obj - next idf.py build will refresh the date.
)

:end
endlocal
