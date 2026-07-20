# ESDeck Media 模式開發筆記

記錄日期:2026-07-20,更新於 2026-07-21
分支:`feature/media-mode`(ESDeck 韌體專案,原名 `feature/media-mode-ui-mock`,後重新命名;此分支會把 Media 功能全部做完再合併回 main)

---

## 0. 現況總覽(2026-07-21 更新,續接請先看這節)

Media 模式的「單一使用者」路徑已經全部做完並可用:真實 Now Playing 資料(歌名/演出者/進度/播放狀態)、播放控制(play/pause/prev/next)、拖曳進度條 seek、封面圖與歌名演出者文字圖傳輸(含真透明背景)、音量視覺化長條、Media 自己的背景圖 + Settings 面板外觀(bg_image + side_icon)三個屬性的 config 系統、MSC 模式互切不再 crash。詳細協定/實作歷程見下面第 1~7 節(原始開發紀錄,保留給查歷史用)。

**目前唯一還沒做、也是下一步要做的功能:多配置切換(config 選取器)。**

背景:第 7 節 config 系統目前是**寫死單一檔案**(`config/media/settings.json`),使用者已明確表示這是刻意先求能測試,之後一定要能像 Deck/Monitor 一樣支援「多組配置 + 選一個當作使用中」。Deck/Monitor 已經有這套機制可以直接抄:

- 用 NVS 存「目前選中哪個配置」(config 名稱或 index)
- SD 卡 `config/<mode>/` 底下可以放多個配置(檔案或子資料夾,需要先看 Deck/Monitor 實際是怎麼分的,下面待確認)
- Settings 選單裡有一個選配置的 UI(列表 + 選取後套用)

**續接時第一步**:去讀 Deck 和 Monitor 兩邊「多配置切換」實際上是怎麼做的(先看 `ui_config.c`/`ui_config_dialog.c` 或對應 monitor 版本,找 NVS key 讀寫、SD 資料夾列舉、選單 UI 這三塊分別在哪個檔案),抓出可以直接複用的模式,再套到 `ui_media_config.c`/`ui_media.c`/`ui_settings.c` 上。**不要重新發明一套新機制**,Media 要跟 Deck/Monitor 長一樣的使用體感。

尚未確認、需要在動工前先查清楚的問題(可以先讀程式碼自己找答案,不確定才問使用者):

1. Deck/Monitor 的「多配置」在 SD 卡上實際的資料夾/檔案佈局長什麼樣子(例如 `config/deck/<name>/settings.json` 還是 `config/deck/<name>.json`)?
2. 選中的配置名稱存在 NVS 的哪個 key、用什麼字串/index 格式?
3. Settings 選單裡選配置的 UI 元件(list popup?)在哪個檔案,能不能直接複用同一個 widget 函式,只是換資料來源?
4. 目前 Media 的 `ui_media_config_load()` 是寫死讀 `config/media/settings.json`,改成多配置後這個函式的介面要怎麼變(加個 config 名稱參數?還是改成先查 NVS 拿名稱再組路徑?)。

---

## 1. Media 是什麼(背景)

正在幫 ESDeck(ESP32-S3 + LVGL,800x480 RGB 螢幕,USB HID 雙向連 PC)加一個新的頂層模式 **Media**,是一個「Now Playing 播放器」畫面,顯示 PC 端目前正在播放的媒體(歌名、演出者、封面圖、進度、播放狀態),並可以從 ESP 端控制播放(play/pause/prev/next/拖曳進度)。

開發分兩階段進行(**兩階段都已完成**):

1. **階段一**:純 UI 原型,先把版面、手感定案,再回頭設計協定,避免協定設計早於 UI 需求導致返工。
2. **階段二**:真的接上 PC 端資料,擴充 HID 協定,拔掉所有假資料。

PC 端專案是 `ESDeckPC`(C# WinForms)。

---

## 2. 現有 HID 雙向協定

自訂 Vendor HID(Usage Page 0xFF00)。IN report(ESP→PC)固定 8 bytes;Feature report(PC→ESP)payload 64 bytes,ESP 端收到時 Report ID 已被 tinyusb 剝掉,所以 ESP 端 `buffer[0]` 對應 PC 端 `report[1]`。

### ESP → PC(IN report:`[byte0, byte1, ...]`)

| byte0 (page) | byte1 (btn) | 觸發點 | PC 端處理 | 說明 |
|---|---|---|---|---|
| 1~N | 1~N | `ui_deck.c` / `ui_monitor.c` 按下頁面按鈕 | `FormM.OnButtonPressed` → `ActionExecutor.Run` | 一般巨集按鈕觸發 |
| 0x00 | key_byte(bit7=shift, bit6:0=HID keycode) | `ui_keyboard.c` 螢幕鍵盤 | 同上 → `ExecKeyboard` | 螢幕鍵盤打字 |
| 0xFF | 0x01 | 進入 monitor 頁 → `usb_hid_monitor_subscribe()` | `MonitorSender.Subscribe()` | 開始收監控資料 |
| 0xFF | 0x02 | 離開 monitor 頁 → `usb_hid_monitor_unsubscribe()` | `MonitorSender.Unsubscribe()` | 停止收監控資料 |
| 0xFF | 0x03 | 回覆 CMD_QUERY,目前非 monitor | `HidReceiver.OnModeReport(false)` | 目前在 deck 模式 |
| 0xFF | 0x04 | 回覆 CMD_QUERY,目前是 monitor | `HidReceiver.OnModeReport(true)` | 目前在 monitor 模式 |
| 0xFE | 0x01 / 0x02 | 進出 Media 頁 | `NowPlayingSender`/`AudioLevelSender`/`NowPlayingImageSender`.Subscribe()/Unsubscribe() | Media 專屬訂閱通道,跟 0xFF 平行、獨立命名空間 |
| 0xFE | 0x03/0x04/0x05 | Media 播放控制按鈕 | `NowPlayingWatcher.TogglePlayPause()/Next()/Previous()` | play_pause/next/prev |
| 0xFE | 0x06 | 拖曳進度條放開 | `NowPlayingWatcher.SeekTo()` | payload 帶 position_ms(4B LE) |

> 註:`usb_hid_release()`(送 0xFF/0xFF)是設計了但沒有任何地方呼叫的死碼。

### PC → ESP(Feature report,ESP 端 `buffer[0]` = cmd)

| cmd | payload | 觸發點 | 說明 |
|---|---|---|---|
| 0x03 CMD_DATA | monitor 數值(cpu/gpu/ram/net/disk 等) | `MonitorSender.WorkerLoop` 每 1s | 系統監控數值 |
| 0x04 CMD_TIME | \[1..6\]=year(-2000)/month/day/hour/min/sec | 同上 | 校時 |
| 0x05 CMD_QUERY | 無 | PC 連上時 | 詢問 ESP 目前模式(已擴充三態 deck/monitor/media) |
| 0x06 CMD_NOWPLAYING_PROGRESS | position(4B)/duration(4B)/playing(1B) | `NowPlayingSender`,狀態變化即送(`AutoResetEvent` + `Nudge()`,不用等滿週期) | 已實作 |
| 0x07 CMD_AUDIO_LEVEL | level(1B, 0-100) | `AudioLevelSender`,10Hz | 已實作,attack 30ms/release 250ms 包絡平滑 |
| 0x08 CMD_NOWPLAYING_IMG_START | generation(1B)/kind(1B)/total_size(2B) | 換歌 / focus 改變時 `NowPlayingImageSender` | 已實作,`total_size=0` 是清空 sentinel |
| 0x09 CMD_NOWPLAYING_IMG_CHUNK | generation(1B)/kind(1B)/data(61B) | 同上 | 已實作 |
| 0x0A CMD_NOWPLAYING_IMG_END | generation(1B)/kind(1B) | 同上 | 已實作,收尾觸發 ESP 端解碼 |

`kind`:0=封面(220x220,JPEG,`esp_new_jpeg` 解碼,不透明 RGB565),1=歌名/演出者文字條(480x70,PNG,LVGL 內建 PNG decoder 解碼,**真透明**背景 RGB565+alpha)。

---

## 3. UI 版面配置

沿用 Deck / Monitor 既有的「每個模式自己接管 sidebar 子區域 + 內容區」架構:

- **Sidebar 子區域**(80 x 400,`s_sidebar_bar_cont`):不放頁籤按鈕,改放一條垂直長條(`s_level_bar`),接的是真實音量資料(§2 的 `CMD_AUDIO_LEVEL`),貼底對齊、無圓角,整條可點擊(關閉 Settings、回到 Media 播放器畫面)。

- **內容區**(720 x 480,`s_page`):播放器卡片,封面圖(220x220,收到真圖前顯示音符 placeholder)、歌名(圖片跑馬灯,PC 端渲染成圖再傳,原因見下)、演出者、`lv_slider` 進度條(可拖曳 seek)、已播放/總長度時間標籤、prev/play-pause/next 三顆按鈕。**支援自訂背景圖**:cover-fit 縮放 + 黑色 50% 透明遮罩(邏輯比照 `ui_deck.c` 的 lazy_bg 慣例),沒設定 `bg_image` 時維持灰底 `0x222222`(跟 Deck/Monitor「沒選背景」時同一個顏色)。無真實資料(未訂閱/斷線 ~3s 逾時)時整組 UI(進度條、三顆按鈕)disable,歌名/演出者顯示「None」,時間顯示「-:--」,封面/文字圖清空回 placeholder。

**中文歌名/字型限制**:ESP 端完全沒有 CJK 字型(`ui.c` 只有 `lv_font_montserrat_16/24` 純西文),所以歌名/演出者不傳文字用 ESP 端字型渲染,改成 PC 端用 GDI+(`Microsoft JhengHei UI`,涵蓋 CJK)畫成一張圖傳過去,跟封面圖走同一套 chunk 傳輸機制。原本 UI 原型用的是 LVGL 原生跑馬灯文字物件,正式版換成圖片後如果要保留跑馬灯效果,原理不變(clip 容器 + timer 改 x 座標),只是操作對象從文字物件換成圖片物件——**目前僅實作靜態文字條,跑馬灯效果尚未套用在圖片版本上**,如果歌名/演出者太長會被裁切,這點跟原本 TODO 一樣尚待決定是否需要。

---

## 4. Config 系統(bg_image / settings 外觀)

Media 沿用 Deck/Monitor 既有的「每個模式自己一份 config JSON」慣例,路徑 `config/media/settings.json`(平行於 `config/deck/`、`config/monitor/`)。

三個屬性:

```json
{
    "bg_image": "sunset.jpg",
    "settings": {
        "bg_image": "panel_bg.jpg",
        "side_icon": "music.png"
    }
}
```

- 頂層 `bg_image`:Media 播放器卡片自己的背景圖
- `settings.bg_image` / `settings.side_icon`:Settings 覆蓋面板(齒輪鈕 + 面板背景)自己的外觀,跟 Deck/Monitor 的 `settings` 物件共用同一個 `ui_settings_appearance_t` 結構

三個欄位都可以省略,省略的欄位維持預設(灰底 `0x222222` / 無自訂 side icon)。檔案不存在或解析失敗時視為「無 config」,不當錯誤處理。

實作檔案:`main/ui/ui_media_config.h`/`.c`(新檔,cJSON 解析,PSRAM 優先配置緩衝區)、`main/app_config.h` 新增 `SD_DIR_CONFIG_MEDIA`/`SD_PATH_CONFIG_MEDIA`、`main/fs_manager/fs_sd.c` 開機自動建立該目錄。

**目前限制(§0 提到的下一步)**:只認一個寫死的檔名,還沒有「多組配置 + 選擇使用中配置」的機制。

---

## 5. 待辦事項

- [ ] **多配置切換**(下一步要做的,見 §0 的規劃)
- [ ] 歌名/演出者圖片版跑馬灯效果(目前是靜態文字條,過長會被裁切)
- [ ] 音頻視覺化「進階版」:FFT 切 8~16 頻段畫頻譜(目前只有單一 VU 值長條)
- [ ] Media 模式自己的「設定」內容(音源靈敏度、視覺化樣式等)——注意這跟 §4 的「外觀 config(bg_image/side_icon)」是不同東西,Deck/Monitor 目前也只有外觀 config dialog,沒有這類功能性設定
- [ ] 封面/文字圖沒有自己的逾時機制,只跟著 progress 斷線一起清掉;如果只是「focus 移到沒有 Media Session 的視窗」但 HID 還連著,圖片會維持上一首內容(進度數字本身仍正常歸零)
- [ ] `usb_hid_release()`(0xFF/0xFF 放開事件)目前是死碼,「長按」之類功能才需要
- [ ] `main/CMakeLists.txt` 的 `file(GLOB_RECURSE ...)` 加 `CONFIGURE_DEPENDS`,避免新增 `ui/*.c` 檔案後忘記 `idf.py reconfigure` 導致連結錯誤(這個修正本身還沒套用,使用者當時選擇手動重編繞過)

---

## 6. WiFi / Bluetooth 評估結論(不採用)

開發商提醒:RGB 屏帶寬高,WiFi 連線時寫 NVS 會造成螢幕閃爍,建議「連上就做完事、別一直連」。討論後結論:

- **WiFi OTA**:不需要,已有 USB MSC 模式可直接把韌體 .bin 寫進 SD 卡
- **天氣小工具**:PC 端 + HID 就能做到,不需要 ESP 自己連 WiFi
- **Bluetooth(僅 BLE)**:手機通知推播、外接 BLE 遙控器討論過,優先度排在 Media 播放器之後
- 結論:WiFi/BLE 目前不推進

---

## 7. 快速上手(回去繼續開發時)

```
cd ESDeck
git checkout feature/media-mode
idf.py reconfigure   # 保險起見,確保新檔案有被抓進 CMake
idf.py -p PORT build flash monitor
```

修改重點檔案速查:

- Media 版面/邏輯 → `main/ui/ui_media.c` / `.h`
- Media config(bg_image/settings)→ `main/ui/ui_media_config.c` / `.h`
- Mode 切換 / Settings 選單 / 根層 X 關閉鈕 → `main/ui/ui_settings.c`
- 三態 mode enum → `main/ui/ui_settings.h`
- HID 協定 → `main/usb/usb_hid.c` / `.h`
- PC 端播放控制/進度 → `ESDeckPC/NowPlayingWatcher.cs`、`NowPlayingSender.cs`
- PC 端封面/文字圖 → `ESDeckPC/NowPlayingImageSender.cs`
- PC 端音量 → `ESDeckPC/AudioLevelWatcher.cs`、`AudioLevelSender.cs`

**續接第一步(多配置切換)**:去讀 Deck/Monitor 的多配置實作(`ui_config.c`/`ui_config_dialog.c` 及 monitor 對應檔案),抓出 NVS key、SD 資料夾佈局、選單 UI 三塊的實際做法,詳細問題清單見 §0。
