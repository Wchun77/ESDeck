# ESDeck Monitor 字體轉換指南

## 環境準備

### 安裝 Node.js

1. 前往 https://nodejs.org/en/download
2. 下載 **Windows Installer (.msi)**，選 LTS 版本
3. 一路 Next 安裝完成
4. 開啟命令提示字元（cmd）確認安裝成功：

```cmd
node -v
npm -v
```

兩個都印出版本號即可。

### 安裝 lv_font_conv

```cmd
npm install -g lv_font_conv
```

確認安裝成功：

```cmd
lv_font_conv --version
```

---

## 字體檔說明

Monitor Clock 頁面使用三個字體，對應不同元素：

| 檔名 | 用途 | 建議尺寸範圍 |
|------|------|-------------|
| `oxanium_270.bin` | 時間（HH:MM） | 200 ~ 300 |
| `oxanium_48.bin` | 秒數 | 32 ~ 64 |
| `oxanium_36.bin` | 日期 + 星期 | 24 ~ 48 |

字體來源：**Oxanium Bold**（Google Fonts，OFL 授權）
下載：https://fonts.google.com/specimen/Oxanium
解壓後取 `Oxanium-Bold.ttf`

---

## 轉換指令

將 `Oxanium-Bold.ttf` 放到一個資料夾，例如 `C:\fonts\`，然後在 cmd 中進入該資料夾：

```cmd
cd C:\fonts
```

### 時間字體（HH:MM，大字）

```cmd
lv_font_conv --font Oxanium-Bold.ttf --size 270 --bpp 4 --range 0x30-0x3A --format bin -o oxanium_270.bin
```

字元範圍包含：`0`–`9` 和 `:`

### 秒數字體（小字）

```cmd
lv_font_conv --font Oxanium-Bold.ttf --size 48 --bpp 4 --range 0x30-0x39 --format bin -o oxanium_48.bin
```

字元範圍包含：`0`–`9`

### 日期 + 星期字體（中字）

```cmd
lv_font_conv --font Oxanium-Bold.ttf --size 36 --bpp 4 -r 0x2F -r 0x30-0x39 -r 0x41-0x5A --format bin -o oxanium_36.bin
```

字元範圍包含：`/`、`0`–`9`、`A`–`Z`

---

## 更換尺寸

想調整字體大小時，只需修改 `--size` 參數，`-o` 的檔名也一起改，避免混淆。

例如把時間字體改成 240px：

```cmd
lv_font_conv --font Oxanium-Bold.ttf --size 240 --bpp 4 --range 0x30-0x3A --format bin -o oxanium_240.bin
```

轉完後在 `monitor.json` 裡更新對應的字體檔名：

```json
{
  "clock": {
    "font_time": "oxanium_240.bin",
    "font_sec":  "oxanium_48.bin",
    "font_date": "oxanium_36.bin"
  }
}
```

---

## 驗證字體

轉換完成後可以用 dump 模式確認字元是否正確：

```cmd
lv_font_conv --font Oxanium-Bold.ttf --size 270 --bpp 4 --range 0x30-0x3A --format dump -o oxanium_270_check
```

會產生一個資料夾，裡面有每個字元的 PNG 預覽圖。

---

## 部署

將轉換好的 `.bin` 檔透過 MSC 模式放進 SD card 的 `assets/fonts/bin/clock/` 資料夾（這層路徑對應
firmware `app_config.h` 裡的 `SD_DIR_ASSETS_FONTS_BIN_CLOCK`；`assets/fonts/bin/` 底下依「功能」分
資料夾，`clock/` 是 Monitor 時鐘專用，`notify/` 是通知用的中文字，各自對應不同的字元集，不要混放）：

```
/sdcard/assets/fonts/bin/clock/oxanium_270.bin
/sdcard/assets/fonts/bin/clock/oxanium_48.bin
/sdcard/assets/fonts/bin/clock/oxanium_36.bin
```

在 Monitor Select Config 選好 config 後，重新套用即生效。

---

## 通知用中文字體（notify.bin）

BLE/ANCS 通知顯示用的中文字體，跟 Monitor 時鐘字體走同一套 `.bin` 機制，**不是**FreeType 動態渲染
——原本想讓使用者自己丟任意 TTF 上去現場用 FreeType 解析，但 FreeType 的堆疊需求沒辦法預先框
死（同一顆字型裡結構複雜的字，堆疊需求可能遠超預期，開機時測過的字通過了，不代表之後手機傳來的
其他字不會讓它爆掉），所以改用跟時鐘字體一樣、先轉換好的點陣字型，只收錄常用字，不開放使用者自訂
上傳任意字型檔。

### 字元集怎麼選

`lv_font_conv` 的 `--range` 只適合連續區間（像數字 `0x30-0x39`），中文常用字在 Unicode 裡是分散
的，要用 `--symbols`（直接給一串字元）或 `--symbols-file`（給一個純文字檔，內容就是要收錄的字）：

```cmd
lv_font_conv --font NotoSansTC-Regular.ttf --size 24 --bpp 4 --symbols-file common_hanzi.txt --format bin -o notify.bin
```

`common_hanzi.txt` 建議用「常用國字標準字體表」或任何高頻字清單（例如前 2000~3000 字），字數
越少，`.bin` 檔案越小、SD 卡佔用越少；生僻字不用收錄，畫到時 LVGL 會顯示內建的缺字佔位符，不是
危險狀況，只是那個字剛好不在收錄範圍內。

英數字元也要一併收錄（App 名稱、時間戳記等常常混著英數），例如：

```cmd
lv_font_conv --font NotoSansTC-Regular.ttf --size 24 --bpp 4 --symbols-file common_hanzi.txt -r 0x20-0x7E --format bin -o notify.bin
```

### 部署

```
/sdcard/assets/fonts/bin/notify/notify.bin
```

對應 firmware `app_config.h` 裡的 `SD_DIR_ASSETS_FONTS_BIN_NOTIFY`。檔名目前是寫死的 `notify.bin`
（第一階段先驗證載入/渲染，之後才會加 Settings 裡的字體選擇功能）。
