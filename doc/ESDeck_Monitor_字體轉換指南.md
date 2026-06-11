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
lv_font_conv --font Oxanium-Bold.ttf --size 36 --bpp 4 --range 0x2F,0x30-0x39,0x41-0x5A --format bin -o oxanium_36.bin
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

將轉換好的 `.bin` 檔透過 MSC 模式放進 SD card 的 `fonts/` 資料夾：

```
/sdcard/fonts/oxanium_270.bin
/sdcard/fonts/oxanium_48.bin
/sdcard/fonts/oxanium_36.bin
```

在 Monitor Select Config 選好 config 後，重新套用即生效。
