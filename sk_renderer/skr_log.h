#pragma once

#include "../include/sk_renderer.h"

#include <stdbool.h>

///////////////////////////////////////////////////////////////////////////////

void skr_log       (skr_log_ level, const char *text);
void skr_logf      (skr_log_ level, const char *text, ...);
void skr_log_enable(bool enabled);
