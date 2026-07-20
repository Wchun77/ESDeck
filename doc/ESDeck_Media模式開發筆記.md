# ESDeck Media 模式開發筆記

記錄日期:2026-07-20
分支:`feature/media-mode`(ESDeck 韌體專案,原名 `feature/media-mode-ui-mock`,後重新命名;此分支會把 Media 功能全部做完再合併回 main)

---

## 1. 現況總覽

正在幫 ESDeck(ESP32-S3 + LVGL,800x480 RGB 螢幕,USB HID 雙向連 PC)加一個新的頂層模式 **Media**,目標是做一個「Now Playing 播放器」畫面。

開發順序刻意分兩階段:

1. **階段一(進行中)**:純 UI 原型,sidebar 動態長條 + 右側播放器卡片全部用假資料模擬,完全不碰 PC 端 / HID 協定。目的是先把版面、手感定案,再回頭設計協定,避免協定設計早於 UI 需求導致返工。
2. **階段二(未開始)**:真的接上 PC 端資料(歌名、封面圖、進度、播放狀態),需要擴充 HID 協定。

目前程式碼**只做了階段一**,PC 端(ESDeckPC,C# WinForms)完全沒有改動。

---

## 2. 現有 HID 雙向協定(未改動,現況紀錄)

自訂 Vendor HID(Usage Page 0xFF00)。IN report(ESP→PC)固定 8 bytes;Feature report(PC→ESP)payload 64 bytes,ESP 端收到時 Report ID 已被 tinyusb 剝掉,所以 ESP 端 `buffer[0]` 對應 PC 端 `report[1]`。

### ESP → PC(IN report:`[byte0, byte1, 保留x6]`)

| byte0 (page) | byte1 (btn) | 觸發點 | PC 端處理 | 說明 |
|---|---|---|---|---|
| 1~N | 1~N | `ui_deck.c` / `ui_monitor.c` 按下頁面按鈕 | `FormM.OnButtonPressed` → `ActionExecutor.Run` | 一般巨集按鈕觸發 |
| 0x00 | key_byte(bit7=shift, bit6:0=HID keycode) | `ui_keyboard.c` 螢幕鍵盤 | 同上 → `ExecKeyboard` | 螢幕鍵盤打字 |
| 0xFF | 0x01 | 進入 monitor 頁 → `usb_hid_monitor_subscribe()` | `MonitorSender.Subscribe()` | 開始收監控資料 |
| 0xFF | 0x02 | 離開 monitor 頁 → `usb_hid_monitor_unsubscribe()` | `MonitorSender.Unsubscribe()` | 停止收監控資料 |
| 0xFF | 0x03 | 回覆 CMD_QUERY,目前非 monitor | `HidReceiver.OnModeReport(false)` | 目前在 deck 模式 |
| 0xFF | 0x04 | 回覆 CMD_QUERY,目前是 monitor | `HidReceiver.OnModeReport(true)` | 目前在 monitor 模式 |

> 註:`usb_hid_release()`(送 0xFF/0xFF)是設計了但**沒有任何地方呼叫**的死碼,目前系統裡不存在「放開」事件。

### PC → ESP(Feature report,ESP 端 `buffer[0]` = cmd)

| cmd | payload | 觸發點 | 說明 |
|---|---|---|---|
| 0x03 CMD_DATA | \[1..13\]=cpu_usage/cpu_temp/ram_usage/gpu_usage/gpu_temp/gpu_vram/cpu_freq(MHz÷100)/net_up/net_down/disk_usage(每10s)/cpu_power/gpu_power/ssd_life(每10s) | `MonitorSender.WorkerLoop` 每 1s | 系統監控數值 |
| 0x04 CMD_TIME | \[1..6\]=year(-2000)/month/day/hour/min/sec | 同上,同一輪一起送 | 校時 |
| 0x05 CMD_QUERY | 無 | PC 連上時 `MonitorSender.SendQuery()` | 詢問 ESP 目前模式 |

現況:IN report 只用了前 2 bytes,Feature report 最多用到 14 bytes,兩邊都還有大量保留空間,加新 cmd 不需要改 HID descriptor。

---

## 3. 階段一實作內容(已完成,在分支上)

### 3.1 檔案異動清單

- `main/ui/ui_settings.h`:`ui_mode_t` 加 `UI_MODE_MEDIA = 2`
- `main/ui/ui_media.h`、`main/ui/ui_media.c`(**新檔**):Media 模式 UI 本體
- `main/ui/ui_deck.h` / `ui_deck.c`:新增 `ui_deck_reselect_current()`
- `main/ui/ui_monitor.h` / `ui_monitor.c`:新增 `ui_monitor_reselect_current()`
- `main/ui/ui_settings.c`:mode 切換邏輯、根層 Settings 按鈕行為、Media 專屬選單項目

`main/CMakeLists.txt` 用 `file(GLOB_RECURSE ... "ui/*.c")` 收檔案,**沒有 `CONFIGURE_DEPENDS`**——新增檔案後如果 `build/` 資料夾是舊的,不會自動被抓進去,會出現連結錯誤(`undefined reference`)。遇到這狀況時執行 `idf.py reconfigure`,或隨便存一下 `main/CMakeLists.txt` 觸發 CMake 重新掃描,或 `idf.py fullclean` 後重建。**這個 CMakeLists.txt 的 `CONFIGURE_DEPENDS` 修正本身還沒套用**(user 當時擋下這個編輯,選擇自己手動重編)。

### 3.2 版面配置

沿用 Deck / Monitor 既有的「每個模式自己接管 sidebar 子區域 + 內容區」架構:

- **Sidebar 子區域**(80 x 400,`s_sidebar_bar_cont`,子物件掛在全域 `sidebar` 上,位置 (0,0)):
  - 不放頁籤按鈕,改放一條**垂直動態長條**(`s_level_bar`,`lv_bar`)
  - 目前用 sine wave + 隨機抖動模擬音量跳動(`fake_timer_cb`,80ms tick)
  - **貼底對齊**(`LV_ALIGN_BOTTOM_MID`),寬 28、高 `SCREEN_H-80-20`,只在上方留 20px,下緣直接頂住 sidebar 底部那塊放 Settings 齒輪鈕的黑色區域
  - **無圓角**(radius 0,MAIN 跟 INDICATOR 都是),避免看起來像浮在中間的橢圓膠囊
  - 整條可點擊(`LV_OBJ_FLAG_CLICKABLE`),點擊會關閉 Settings 並回到 Media 播放器畫面(因為 Media 沒有頁籤按鈕可以點來離開設定,詳見 3.3)

- **內容區**(720 x 480,`s_page`,子物件掛在 `lv_scr_act()`,位置 (80,0)):
  - 播放器卡片:封面 placeholder(灰底圓角方塊 + 音符 icon)、歌名(`LV_LABEL_LONG_SCROLL_CIRCULAR` 原生跑馬灯)、演出者、進度條、已播放/總長度時間標籤、prev/play-pause/next 三顆按鈕
  - 內建 3 首假歌曲(其中一首標題刻意拉很長,用來預覽跑馬灯效果),next/prev 會循環切換
  - **進度條是 `lv_slider`,不是 `lv_bar`**:`lv_bar` 唯讀不能拖,`lv_slider` 才能拖曳拖拉快轉。knob 視覺做小(padding 4),track 寬 480、置中對齊時間標籤(±240),拖曳時 `s_seeking` flag 會暫停假資料的自動進度更新,放開後重新同步
  - 目前是**純平面深色底**,還沒有背景圖支援
  - **TODO(使用者已提出,尚未實作)**:之後接上真的 bg_image 時,要比照 `ui_deck.c` create_page() / `ui_monitor.c` make_page() 的 bg(child0) + mask(child1) 慣例,加一層半透明遮罩,讓播放器卡片在複雜背景圖上還能看清楚。目前程式碼裡已經留了 TODO 註解在 `ui_media.c` 的 `build_player_card()` 開頭。

### 3.3 Mode 切換邏輯(`ui_settings.c`)

- `UI_MODE_MEDIA` 走跟 Deck↔Monitor 一樣的 **switching screen + 背景 xTaskCreate** 流程(`enter_media_task` / `on_enter_media_done`,延遲 1 秒模擬未來讀取背景圖/icon 的時間),不是原本階段一開始寫的同步瞬間切換。
- Settings 選單新增三個項目:
  - 「Switch to Media」:Deck、Monitor 都看得到(`item_mode_to_media_cb`)
  - 「Switch to Monitor」:只在 Media 底下看得到(`item_mode_from_media_to_monitor_cb`,重用既有 `enter_monitor_task`)
  - 「Switch to Deck」:只在 Media 底下看得到(`item_mode_from_media_cb`)
- 「Boot Animation」「System」子選單裡的「Switch to MSC mode」「Info」也一起開放給 Media 用(這些跟模式無關,新增 `SETMASK_ALL = SETMASK_DECK|SETMASK_MONITOR|SETMASK_MEDIA`)
- **Settings 根層的返回鈕行為改了,三個模式都受影響**:原本只有巢狀選單(depth>0)時左上角才顯示「←」返回鈕,根層(depth==0)是隱藏的——因為 Deck/Monitor 原始設計是「點別的頁籤按鈕就等於離開設定」。Media 沒有頁籤按鈕可點,所以改成:根層時返回鈕顯示成「✕」(`LV_SYMBOL_CLOSE`),點擊會 `close_settings()`:呼叫 `ui_settings_deselect()` 後,依目前 `s_mode` 呼叫對應的 `ui_deck_reselect_current()` / `ui_monitor_reselect_current()` / `ui_media_reselect_current()`,把原本畫面的內容區重新顯示出來。這是**三個模式共用的新機制**,不只是 Media 專屬。

---

## 4. Now Playing 資料傳輸設計討論(僅討論,尚未寫成程式)

### 4.1 中文歌名問題

裝置上目前**完全沒有 CJK 字型**,`ui.c` 只用 `lv_font_montserrat_16/24`(純西文),PC 端 `FormFontBuilder.cs` 建的字型也只有時鐘用的窄範圍(數字、大寫字母)。結論:**不要把歌名文字傳給 ESP 端字型渲染**,改成 PC 端用 GDI+(任何語言字型都能畫)把歌名/演出者渲染成一張小圖,跟封面圖走同一套傳輸機制。

### 4.2 跑馬灯效果怎麼保留

LVGL 原生跑馬灯(`LV_LABEL_LONG_SCROLL_CIRCULAR`)本質是「固定寬度 clip 容器 + `lv_anim` 持續改變內部物件 x 座標,循環模式會把內容多接一份做無縫循環」。這個機制不挑物件種類,套在 `lv_img` 上一樣可行:PC 端把歌名畫成一條「比顯示區寬」的圖(必要時尾端重複一次文字做無縫循環),傳過去後 ESP 端一樣用 timer 改 x 座標、外層 clip,效果跟原生文字跑馬灯一致,只是底層是圖片不是文字物件。

> 目前 UI 原型的歌名跑馬灯(`ui_media.c` `s_title_lbl`)用的是 **LVGL 原生 `LV_LABEL_LONG_SCROLL_CIRCULAR`**,純粹是先用假的英文字串預覽跑馬灯「感覺」,正式版才會換成上面說的圖片方案(檔案內已有註解說明這個差異)。

### 4.3 封面圖傳輸(HID 頻寬考量)

- 不走 SD 卡:不要求 PC 端在播放前先把封面圖寫進 SD 卡(SD 卡目前用途已經很多,而且事先不知道使用者要播哪首歌)
- 直接複用開機動畫已經在用的 `esp_new_jpeg` 解碼器:PC 端把封面壓縮/縮小成小尺寸 JPEG(例如 120~160px 見方,不需要原始解析度),透過 HID chunk 傳過去,ESP 收完直接解碼丟進 LVGL image object,歌換了就丟棄重解,全程在記憶體裡,不碰 SD
- 估算:縮到 160x160、壓縮到幾 KB 的 JPEG,就算單次 HID report 只有 64 bytes,幾 KB 也就一兩秒內能傳完
- **切歌打斷處理**:每次「現在播放」事件給一個 generation_id,圖片 chunk 也帶這個 id;序號中途變了(代表換歌)就丟棄舊資料,等新序號的 chunk 到齊再解碼。沒收到新圖之前畫面顯示固定音符 placeholder。

### 4.4 提議的新 cmd byte(尚未定案、尚未實作)

延續現有協定慣例(page/cmd byte 從保留值繼續往下配):

**ESP → PC**:新增 `page = 0xFE` 當作 Media 專屬控制通道(跟現有 `0xFF` monitor 控制是平行、不相交的獨立命名空間),`btn=0x01`/`0x02` 分別是訂閱/取消訂閱(進出 Media 頁面時送,跟 Monitor 那套訂閱行為模式一樣,但底層是獨立的)。

**PC → ESP**:延續現有 `0x03/0x04/0x05` 之後:

| cmd | payload 概念 | 說明 | 狀態 |
|---|---|---|---|
| 0x06 CMD_NOWPLAYING_PROGRESS | position(4B)/duration(4B)/playing(1B) | 數字類,小,每秒送 | **已實作** |
| 0x07 CMD_AUDIO_LEVEL | level(1B, 0-100) | 第 5 節音頻視覺化「簡單版」VU 值,10Hz 送 | **已實作** |
| 0x08 CMD_NOWPLAYING_IMG_START | generation_id(1B)/total_size(2B)/kind(1B:封面 or 文字條) | 宣告一次圖片傳輸開始 | 未實作 |
| 0x09 CMD_NOWPLAYING_IMG_CHUNK | generation_id(1B)/chunk_idx(2B)/data(...) | 圖片分塊 | 未實作 |
| 0x0A CMD_NOWPLAYING_IMG_END | generation_id(1B) | 收尾,通知 ESP 可以解碼顯示了 | 未實作 |

> cmd byte 編號已依實作結果調整:原本規劃 0x07 給圖片傳輸開頭用,但音量 VU 值(第 5 節)先落地,所以圖片相關三個 cmd 往後遞補到 0x08-0x0A。

`kind` 用來區分這塊是「封面圖」還是「歌名/演出者渲染出來的文字圖」,兩種走同一套 chunk 機制,只是貼的位置跟圖片尺寸不同。

---

## 5. 音頻視覺化(動態長條)討論

- ESP 沒有麥克風,聲音處理全部在 PC 端做,ESP 只負責拿數字畫圖
- PC 端可用 WASAPI loopback(例如 NAudio 的 `WasapiLoopbackCapture`)「聽」系統輸出音訊,不需要實體麥克風
- **簡單版**:單一音量值(VU meter),每次送 1 byte,ESP 畫呼吸圓圈/跳動長條類的圖形——目前 UI 原型的 sidebar 長條就是這個的假資料版本
- **進階版**:FFT 切 8~16 個頻段,每段一個 byte,64-byte report 綽綽有餘,ESP 畫成長條頻譜
- 更新頻率建議 20~30Hz 才會看起來夠順,比照 Now Playing 的訂閱模式,只有使用者打開視覺化頁面時才拉高送資料頻率
- **PC 端目前完全沒有音訊相關 library(NAudio 等都沒裝)**,這塊是全新依賴

---

## 6. WiFi / Bluetooth 評估結論(不採用)

開發商提醒:RGB 屏帶寬高,WiFi 連線時寫 NVS 會造成螢幕閃爍,建議「連上就做完事、別一直連」。討論後的結論:

- **WiFi OTA**:不需要,ESP 已經有 USB MSC 模式,PC 可以直接把韌體 .bin 寫進 SD 卡,不用插拔卡片,WiFi OTA 沒有比現有方式方便
- **天氣小工具**:PC 端 + HID 就能做到(PC 抓天氣資料轉送過來),不需要 ESP 自己連 WiFi
- **Bluetooth(僅 BLE,ESP32-S3 沒有 Classic BT)**:手機通知推播、外接 BLE 遙控器等想法討論過,但目前沒有明確吸引力/急迫性,優先度排在 Media 播放器之後
- 結論:**WiFi/BLE 這條線目前不推進**,先把 Media 播放器做完整

---

## 7. 待辦事項(Follow-ups)

- [ ] `main/CMakeLists.txt` 的 `file(GLOB_RECURSE ...)` 加上 `CONFIGURE_DEPENDS`,避免以後新增 `ui/*.c` 檔案又忘記重新 configure(這次遇到的連結錯誤就是這個原因)
- [ ] Media 內容區加半透明背景遮罩(等真的有 bg_image 支援時一起做,`ui_media.c` 已留 TODO 註解)
- [x] 第 4.4 節 Now Playing HID 協定(數字部分)-- **已實作**,而且已經拔掉階段一的 UI 原型假資料(mock track 清單、sine wave、本地計時器全部刪除):
  - `HID_MEDIA_CMD_NOWPLAYING_PROGRESS=0x06`(position/duration/playing)+ `page=0xFE` 訂閱通道。PC 端 `NowPlayingWatcher`(讀 Media Session API,`Position` 用 `LastUpdatedTime` 內插成即時值,避免 Windows 只在離散事件推播位置造成的偏移/凍結)+ `NowPlayingSender`(每秒送 HID)。
  - `CMD_QUERY` 已擴充成三態(deck/monitor/media,`usb_hid_reply_mode()`),PC 端 App 比較晚啟動、ESP 已經在 Media 頁的情況會自動補訂閱。
  - **ESP → PC 播放控制已實作**:新增 `HID_MEDIA_BTN_PLAY_PAUSE/NEXT/PREV`(0x03-0x05,同一個 `page=0xFE` 通道),ESP 端按鈕按下去送指令給 PC,PC 端 `NowPlayingWatcher.TogglePlayPause()/Next()/Previous()` 呼叫 `GlobalSystemMediaTransportControlsSession` 的對應方法真的控制播放器;按鈕本身不做本地樂觀更新,狀態一律等下一筆真資料回來才變。
  - **連線狀態鎖定**:`ui_media.c` 沒收到真資料(未訂閱成功、或 ~3s 逾時)時,歌名/演出者顯示「None」,時間顯示「-:--」,進度條與 prev/play/next 三顆按鈕整組 disable(不可拖曳/不可按),不再用假資料撐著看起來像在動。
  - **尚未實作**:歌名/演出者/封面圖(cmd 已改配 0x08-0x0A,PC 端 GDI+ 畫文字條 + JPEG 縮圖 pipeline)、拖曳進度條 seek(目前放開手只是等下一筆真資料校正,沒有送 seek 指令回 PC)
- [x] 第 5 節音頻視覺化「簡單版」-- **已實作**,同樣拔掉了原本的 sine wave 假資料:
  - `CMD_AUDIO_LEVEL=0x07`,PC 端 `AudioLevelWatcher`(WASAPI loopback)+ `AudioLevelSender`(10Hz,跟 Now Playing 共用 0xFE 訂閱通道)。
  - **平滑處理**:PC 端加了 attack/release 包絡(攻擊 30ms、釋放 250ms 的指數平滑),不是每個 buffer 的瞬間 peak 直接送,避免數值跳得生硬;ESP 端 `ui_media.c` 的 level bar 改用 `LV_ANIM_ON` + `lv_bar_set_anim_time(120)`,收到新值時用補間動畫過渡,不是瞬間跳格。
  - 沒收到真資料(或 ~3s 逾時)時 bar 直接歸零,不再退回 sine wave。
  - **尚未**:FFT 頻段進階版(§5「進階版」)
- [ ] Media 模式的「設定」畫面(音源靈敏度、視覺化樣式等)目前完全沒有,只有 Deck/Monitor 有自己的 config dialog
- [ ] `usb_hid_release()`(0xFF/0xFF 放開事件)目前是死碼,如果之後要做「長按」之類的功能才需要真的接上

---

## 8. 快速上手(回去繼續開發時)

```
cd ESDeck
git checkout feature/media-mode
idf.py reconfigure   # 保險起見,確保新檔案有被抓進 CMake
idf.py -p PORT build flash monitor
```

修改重點檔案速查:

- Media 版面/假資料邏輯 → `main/ui/ui_media.c`
- Mode 切換 / Settings 選單 / 根層 X 關閉鈕 → `main/ui/ui_settings.c`
- 三態 mode enum → `main/ui/ui_settings.h`
- HID 協定(現況,尚未擴充)→ `main/usb/usb_hid.c` / `.h`
