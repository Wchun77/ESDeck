@echo off
REM 強制下一次 idf.py build 重新產生 esp_app_desc_t 裡的編譯日期/時間。
REM
REM 這個時間戳記(Info頁面顯示的"Built ..."字樣)是編譯esp_app_format
REM 元件裡的esp_app_desc.c那一刻寫進二進位檔的。平常改main.c或其他
REM 程式碼不會讓ninja判定這個檔案需要重新編譯,結果就是即使整個.bin
REM 有重新link過,裡面嵌的日期還是舊的。刪掉編譯產物(.obj)強迫下一次
REM build只重新編譯這一個小檔案+重新link,速度幾乎沒差,但日期會準確
REM 反映當次建置時間。
REM
REM 放在 tools\ 底下,用相對路徑找 ..\build\ 下的產物,在專案根目錄的
REM build 資料夾存在時才會動作。

setlocal

set "TARGET=%~dp0..\build\esp-idf\esp_app_format\CMakeFiles\__idf_esp_app_format.dir\esp_app_desc.c.obj"

if not exist "%TARGET%" (
    echo [skip] 找不到 %TARGET%
    echo        可能還沒build過,或路徑跟預期的build目錄結構不同。
    goto :end
)

del /f /q "%TARGET%"
if errorlevel 1 (
    echo [error] 刪除失敗,請確認檔案沒有被其他程式(例如IDE正在build)佔用。
) else (
    echo [ok] 已清除 esp_app_desc.c.obj,下次 idf.py build 會重新產生正確的編譯日期。
)

:end
endlocal
