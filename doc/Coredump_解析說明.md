# 如何解析 coredump .elf

裝置當機後,dump 會自動存到 SD 卡的 `.dump\coredump_NNNN.elf`。要看內容需要用 ESP-IDF 的工具解析。

## 步驟

1. 打開 VSCode,`Ctrl+Shift+P` 執行 **「ESP-IDF: Open ESP-IDF Terminal」**

2. 在跳出的終端機裡執行(路徑改成你自己的 .elf 檔位置):

   ```
   idf.py coredump-info -c "F:\.dump\coredump_0000.elf"
   ```

3. 終端機會直接印出當機原因、call stack、各 task 狀態等文字結果。

## 注意

- 要在 ESDeck 專案目錄下執行(VSCode 開的終端機預設就在專案目錄,不用額外 `cd`),工具才找得到 `build\ESDeck.elf` 來對照函式名稱、行號。
- `build\ESDeck.elf` 必須跟「當機當下燒進去的那個版本」一致。如果之後又重新 build 過(改過程式碼或 sdkconfig),解析結果可能對不上、甚至報錯——這代表版本不同步,不是工具壞掉。
