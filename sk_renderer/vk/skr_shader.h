// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#pragma once

#include "sk_renderer.h"
#include "skr_vulkan.h"

///////////////////////////////////////////////////////////////////////////////

skr_shader_stage_t _skr_shader_stage_create     (const void* shader_data, uint32_t shader_size, skr_stage_ type);
void               _skr_shader_stage_destroy    (skr_shader_stage_t* stage);
skr_shader_stage_t _skr_shader_file_create_stage(const sksc_shader_file_t *file, skr_stage_ stage);
skr_shader_t       _skr_shader_create_manual    (sksc_shader_meta_t*  meta, skr_shader_stage_t v_shader, skr_shader_stage_t p_shader, skr_shader_stage_t c_shader);