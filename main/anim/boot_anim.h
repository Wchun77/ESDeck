#pragma once
#include <stdbool.h>

#define BOOT_ANIM_STYLE 2

/* 在背景task先把esp_new_jpeg解碼器暖機好(第一次呼叫jpeg_dec_open()有
 * 明顯的一次性初始化成本,實測約1秒)。建議在main.c裡越早呼叫越好,
 * 讓它跟SD卡掛載/preload等其他開機工作重疊進行,這樣boot_anim_play()
 * 真正要畫第一幀時就不用再付這筆初始化成本。就算沒呼叫這個函式也沒
 * 關係,decode第一幀時會自動補做初始化,只是會變成同步等待。 */
void boot_anim_prewarm_jpeg(void);

/* 播開機動畫(NVS CFG_NVS_KEY_BOOT_ANIM:SD上的資料夾名稱 / "default"或
 * 沒設定 = 內建程序動畫 / "none" = 不播)。有播的話會開背光並回傳true;
 * "none"回傳false,背光維持關閉,由呼叫端在UI畫好後自己開。 */
bool boot_anim_play(void);