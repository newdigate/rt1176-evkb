/* vgc_cases_color.cpp - colour and blend cases (Phase 2 spec section 3).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#include "vgc_harness.h"

/* One never-iterated entry against a zero count, the same idiom and for the
 * same reason as vgc_dangerous.cpp's OFF branch -- see its comment. The
 * harness's null-entry guard makes even a drifted count a transcript line
 * rather than a fault. */
const vgc_case_t vgc_color_cases[] = { { NULL, NULL, NULL, NULL } };
const size_t     vgc_color_case_count = 0;
