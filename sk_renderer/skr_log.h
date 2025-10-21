// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#pragma once

#include "../include/sk_renderer.h"

#include <stdbool.h>

///////////////////////////////////////////////////////////////////////////////

void skr_log       (skr_log_ level, const char *text);
void skr_logf      (skr_log_ level, const char *text, ...);
void skr_log_enable(bool enabled);
