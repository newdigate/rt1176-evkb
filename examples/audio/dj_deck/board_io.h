/* board_io.h - the board under the loader: the SD card as Storage, SDRAM as
 * its memory, micros() as its clock.
 * Copyright (c) 2026 Nicholas Newdigate.  SPDX-License-Identifier: MIT */
#pragma once
#include "loader.h"

bool board_sd_begin();                 /* SD.begin(BUILTIN_SDCARD); safe to call again after a failure */
Storage &board_storage();
LoaderMem board_mem();                 /* extmem_malloc / extmem_free */
uint32_t board_now_us();
