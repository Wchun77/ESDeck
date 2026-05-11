#pragma once

void ui_preload_start(void);  /* load config + start background preload task */
void ui_preload_wait(void);   /* block until preload task finishes */
void my_ui_init(void);