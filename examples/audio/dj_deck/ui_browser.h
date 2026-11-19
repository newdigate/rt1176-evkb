/* ui_browser.h - the file browser screen: folders and audio files on the SD card.
 * Copyright (c) 2026 Nicholas Newdigate.  SPDX-License-Identifier: MIT */
#pragma once
#include <lvgl.h>

struct BrowserCallbacks { void (*load)(const char *path); void (*back)(); };

void ui_browser_create(lv_obj_t *screen, BrowserCallbacks cb);
void ui_browser_show();                /* (re)list the current folder; "/" the first time */
void ui_browser_tick();                /* fills in one row's duration and BPM per call */
