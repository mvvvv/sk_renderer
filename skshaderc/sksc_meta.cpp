#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX

#include "_sksc.h"
#include "array.h"

#include <string.h>

//#include <spirv_hlsl.hpp>
#include <spirv_reflect.h>

///////////////////////////////////////////

void sksc_line_col (const char *from_text, const char *at, int32_t *out_line, int32_t *out_column);
int  strcmp_nocase (char const *a, char const *b);
void parse_semantic(const char* str, char* out_str, int32_t* out_idx);

///////////////////////////////////////////

bool sksc_spirv_to_meta(const sksc_shader_file_stage_t *spirv_stage, sksc_shader_meta_t *ref_meta) {
	// Create reflection data
	SpvReflectShaderModule module;
	SpvReflectResult       result = spvReflectCreateShaderModule(spirv_stage->code_size, spirv_stage->code, &module);
	
	if (result != SPV_REFLECT_RESULT_SUCCESS) {
		sksc_log(log_level_err, "[SPIRV-Reflect] Failed to create shader module: %d", result);
		return false;
	}

	array_t<sksc_shader_buffer_t> buffer_list = {};
	buffer_list.data      = ref_meta->buffers;
	buffer_list.capacity  = ref_meta->buffer_count;
	buffer_list.count     = ref_meta->buffer_count;
	array_t<sksc_shader_resource_t> resource_list = {};
	resource_list.data     = ref_meta->resources;
	resource_list.capacity = ref_meta->resource_count;
	resource_list.count    = ref_meta->resource_count;

	// Get descriptor bindings
	uint32_t binding_count = 0;
	result = spvReflectEnumerateDescriptorBindings(&module, &binding_count, nullptr);
	if (result != SPV_REFLECT_RESULT_SUCCESS) {
		spvReflectDestroyShaderModule(&module);
		return false;
	}

	SpvReflectDescriptorBinding** bindings = (SpvReflectDescriptorBinding**)malloc(sizeof(SpvReflectDescriptorBinding*) * binding_count);
	result = spvReflectEnumerateDescriptorBindings(&module, &binding_count, bindings);
	if (result != SPV_REFLECT_RESULT_SUCCESS) {
		spvReflectDestroyShaderModule(&module);
		return false;
	}

	// Process uniform buffers
	for (uint32_t i = 0; i < binding_count; i++) {
		SpvReflectDescriptorBinding* binding = bindings[i];
		
		if (binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
			// Find or create a buffer
			const char* buffer_name = binding->type_description->type_name
				? binding->type_description->type_name 
				: (binding->name ? binding->name : "");

			int64_t id = buffer_list.index_where([](const sksc_shader_buffer_t &buff, void *data) { 
				return strcmp(buff.name, (char*)data) == 0; 
			}, (void*)buffer_name);
			bool is_new = id == -1;
			if (is_new) id = buffer_list.add({});

			// Update the stage of this buffer
			sksc_shader_buffer_t *buff = &buffer_list[id];
			buff->bind.stage_bits |= spirv_stage->stage;

			// And skip the rest if we've already seen it
			if (!is_new) continue;

			SpvReflectTypeDescription* type_desc = binding->type_description;
			uint32_t count = type_desc->member_count;

			buff->size               = (uint32_t)(binding->block.size % 16 == 0 ? binding->block.size : (binding->block.size / 16 + 1) * 16);
			buff->space              = binding->set;
			buff->bind.slot          = binding->binding;
			buff->bind.stage_bits    = spirv_stage->stage;
			buff->bind.register_type = skr_register_constant;
			buff->var_count          = count;
			buff->vars               = (sksc_shader_var_t*)malloc(count * sizeof(sksc_shader_var_t));
			memset(buff->vars, 0, count * sizeof(sksc_shader_var_t));
			strncpy(buff->name, buffer_name, sizeof(buff->name));

			for (uint32_t m = 0; m < count; m++) {
				SpvReflectBlockVariable* member = &binding->block.members[m];
				
				uint32_t dimensions = member->array.dims_count;
				int32_t  dim_size   = 1;
				for (uint32_t d = 0; d < dimensions; d++) {
					dim_size = dim_size * member->array.dims[d];
				}
				
				const char* member_name = member->name ? member->name : "";
				strncpy(buff->vars[m].name, member_name, sizeof(buff->vars[m].name));
				buff->vars[m].offset     = member->offset;
				buff->vars[m].size       = member->size;
				
				uint32_t vec_size = member->type_description->traits.numeric.vector.component_count;
				uint32_t columns  = member->type_description->traits.numeric.matrix.column_count;
				if (vec_size == 0) vec_size = 1;
				if (columns  == 0) columns  = 1;
				
				buff->vars[m].type_count = dim_size * vec_size * columns;
				
				if (buff->vars[m].type_count == 0)
					buff->vars[m].type_count = 1;
				
				switch (member->type_description->type_flags & 0xFF) {
					case SPV_REFLECT_TYPE_FLAG_INT:
						if (member->type_description->traits.numeric.scalar.signedness) {
							buff->vars[m].type = sksc_shader_var_int;
						} else {
							if (member->type_description->traits.numeric.scalar.width == 8)
								buff->vars[m].type = sksc_shader_var_uint8;
							else
								buff->vars[m].type = sksc_shader_var_uint;
						}
						break;
					case SPV_REFLECT_TYPE_FLAG_FLOAT:
						if (member->type_description->traits.numeric.scalar.width == 64)
							buff->vars[m].type = sksc_shader_var_double;
						else
							buff->vars[m].type = sksc_shader_var_float;
						break;
					default:
						buff->vars[m].type = sksc_shader_var_none;
						break;
				}
			}
			
			if (strcmp(buff->name, "$Global") == 0) {
				ref_meta->global_buffer_id = (int32_t)id;
			}
		}
	}

	// Find textures (sampled images)
	for (uint32_t i = 0; i < binding_count; i++) {
		SpvReflectDescriptorBinding* binding = bindings[i];
		
		if (binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
			const char* name = binding->name ? binding->name : "";
			int64_t id = resource_list.index_where([](auto &tex, void *data) { 
				return strcmp(tex.name, (char*)data) == 0; 
			}, (void*)name);
			if (id == -1)
				id = resource_list.add({});

			sksc_shader_resource_t *tex = &resource_list[id]; 
			tex->bind.slot          = binding->binding;
			tex->bind.stage_bits   |= spirv_stage->stage;
			tex->bind.register_type = skr_register_texture;
			strncpy(tex->name, name, sizeof(tex->name));
		}
	}

	// Look for storage images (RWTexture2D)
	for (uint32_t i = 0; i < binding_count; i++) {
		SpvReflectDescriptorBinding* binding = bindings[i];
		
		if (binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
			const char* name = binding->name ? binding->name : "";
			int64_t id = resource_list.index_where([](auto &tex, void *data) { 
				return strcmp(tex.name, (char*)data) == 0; 
			}, (void*)name);
			if (id == -1)
				id = resource_list.add({});

			sksc_shader_resource_t *tex = &resource_list[id];
			tex->bind.slot          = binding->binding;
			tex->bind.stage_bits   |= spirv_stage->stage;
			tex->bind.register_type = skr_register_readwrite_tex;
			strncpy(tex->name, name, sizeof(tex->name));
		}
	}

	// Look for storage buffers (RWStructuredBuffers and StructuredBuffers)
	for (uint32_t i = 0; i < binding_count; i++) {
		SpvReflectDescriptorBinding* binding = bindings[i];
		
		if (binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
			const char* name = binding->name ? binding->name : "";
			
			// Check if readonly by looking at type flags
			bool readonly = (binding->type_description->type_flags       & SPV_REFLECT_TYPE_FLAG_EXTERNAL_BLOCK) &&
			               !(binding->type_description->decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE);
			// Note: may need to adjust the readonly detection based on your SPIRV generation
			
			int64_t id = resource_list.index_where([](auto &tex, void *data) { 
				return strcmp(tex.name, (char*)data) == 0; 
			}, (void*)name);
			if (id == -1)
				id = resource_list.add({});

			sksc_shader_resource_t *tex = &resource_list[id];
			tex->bind.slot          = binding->binding;
			tex->bind.stage_bits   |= spirv_stage->stage;
			tex->bind.register_type = readonly ? skr_register_read_buffer : skr_register_readwrite;
			strncpy(tex->name, name, sizeof(tex->name));
		}
	}

	free(bindings);

	// Get vertex input info
	if (spirv_stage->stage == skr_stage_vertex) {
		uint32_t input_count = 0;
		result = spvReflectEnumerateInputVariables(&module, &input_count, nullptr);
		if (result != SPV_REFLECT_RESULT_SUCCESS) {
			spvReflectDestroyShaderModule(&module);
			return false;
		}

		SpvReflectInterfaceVariable** inputs = (SpvReflectInterfaceVariable**)malloc(sizeof(SpvReflectInterfaceVariable*) * input_count);
		result = spvReflectEnumerateInputVariables(&module, &input_count, inputs);
		if (result != SPV_REFLECT_RESULT_SUCCESS) {
			spvReflectDestroyShaderModule(&module);
			return false;
		}

		ref_meta->vertex_input_count = 0;
		ref_meta->vertex_inputs = (skr_vert_component_t*)malloc(sizeof(skr_vert_component_t) * input_count);

		int32_t curr = 0;
		for (uint32_t i = 0; i < input_count; i++) {
			SpvReflectInterfaceVariable* input = inputs[i];
			
			// Skip built-ins
			if (input->built_in != (SpvBuiltIn)0xFFFFFFFF) {
				continue;
			}

			const char* semantic_str = input->semantic ? input->semantic : "";
			
			char    semantic[64];
			int32_t semantic_idx = 0;
			parse_semantic(semantic_str, semantic, &semantic_idx);

			if (strlen(semantic) > 3 &&
				tolower(semantic[0]) == 's' &&
				tolower(semantic[1]) == 'v' &&
				tolower(semantic[2]) == '_' &&
				strcmp_nocase(semantic, "sv_position") != 0)
			{
				continue;
			}

			ref_meta->vertex_inputs[curr].semantic_slot = semantic_idx;
			if      (strcmp_nocase(semantic, "sv_position" ) == 0) { ref_meta->vertex_inputs[curr].semantic = skr_semantic_position;     }
			else if (strcmp_nocase(semantic, "binormal"    ) == 0) { ref_meta->vertex_inputs[curr].semantic = skr_semantic_binormal;     }
			else if (strcmp_nocase(semantic, "blendindices") == 0) { ref_meta->vertex_inputs[curr].semantic = skr_semantic_blendindices; }
			else if (strcmp_nocase(semantic, "blendweight" ) == 0) { ref_meta->vertex_inputs[curr].semantic = skr_semantic_blendweight;  }
			else if (strcmp_nocase(semantic, "color"       ) == 0) { ref_meta->vertex_inputs[curr].semantic = skr_semantic_color;        }
			else if (strcmp_nocase(semantic, "normal"      ) == 0) { ref_meta->vertex_inputs[curr].semantic = skr_semantic_normal;       }
			else if (strcmp_nocase(semantic, "position"    ) == 0) { ref_meta->vertex_inputs[curr].semantic = skr_semantic_position;     }
			else if (strcmp_nocase(semantic, "psize"       ) == 0) { ref_meta->vertex_inputs[curr].semantic = skr_semantic_psize;        }
			else if (strcmp_nocase(semantic, "tangent"     ) == 0) { ref_meta->vertex_inputs[curr].semantic = skr_semantic_tangent;      }
			else if (strcmp_nocase(semantic, "texcoord"    ) == 0) { ref_meta->vertex_inputs[curr].semantic = skr_semantic_texcoord;     }

			uint32_t vec_size = input->type_description->traits.numeric.vector.component_count;
			ref_meta->vertex_inputs[curr].count = vec_size > 0 ? vec_size : 1;
			
			switch (input->type_description->type_flags & 0xFF) {
				case SPV_REFLECT_TYPE_FLAG_FLOAT: ref_meta->vertex_inputs[curr].format = skr_vertex_fmt_f32;  break;
				case SPV_REFLECT_TYPE_FLAG_INT:
					if (input->type_description->traits.numeric.scalar.signedness)
						ref_meta->vertex_inputs[curr].format = skr_vertex_fmt_i32;
					else
						ref_meta->vertex_inputs[curr].format = skr_vertex_fmt_ui32;
					break;
				default: ref_meta->vertex_inputs[curr].format = skr_vertex_fmt_none; break;
			}
			curr += 1;
		}
		free(inputs);
		ref_meta->vertex_input_count = curr;
	}

	ref_meta->buffers        = buffer_list.data;
	ref_meta->buffer_count   = (uint32_t)buffer_list.count;
	ref_meta->resources      = resource_list.data;
	ref_meta->resource_count = (uint32_t)resource_list.count;

	spvReflectDestroyShaderModule(&module);
	return true;
}

///////////////////////////////////////////

void parse_semantic(const char* str, char* out_str, int32_t* out_idx) {
	const char *curr  = str;
	char*       write = out_str;
	int         idx   = 0;
	while (*curr != 0) {
		if (*curr>='0' && *curr<='9') {
			idx  = idx * 10;
			idx += (*curr) - '0';
		} else {
			*write = *curr;
			write++;
		}
		curr++;
	}
	*write   = '\0';
	*out_idx = idx;
}

///////////////////////////////////////////

int strcmp_nocase(char const *a, char const *b) {
	for (;; a++, b++) {
		int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
		if (d != 0 || !*a)
			return d;
	}
}

///////////////////////////////////////////

int64_t mini(int64_t a, int64_t b) {return a<b?a:b;}
int64_t maxi(int64_t a, int64_t b) {return a>b?a:b;}

///////////////////////////////////////////

array_t<sksc_meta_item_t> sksc_meta_find_defaults(const char *hlsl_text) {
	// Searches for metadata in comments that look like this:
	//--name                 = unlit/test
	//--time: color          = 1,1,1,1
	//--tex: 2D, external    = white
	//--uv_scale: range(0,2) = 0.5
	// Where --name is a unique keyword indicating the shader's name, and
	// other elements follow the pattern of:
	// |indicator|param name|tag separator|tag string|default separator|comma separated default values
	//  --        time       :             color      =                 1,1,1,1
	// Metadata can be in // as well as /**/ comments

	array_t<sksc_meta_item_t> items = {};

	// This function will get each line of comment from the file
	const char *(*next_comment)(const char *src, const char **ref_end, bool *ref_state) = [](const char *src, const char **ref_end, bool *ref_state) {
		const char *c      = *ref_end == nullptr ? src : *ref_end;
		const char *result = nullptr;

		// If we're inside a /**/ block, continue from the previous line, we
		// just need to skip any newline characters at the end.
		if (*ref_state) {
			result = (*ref_end)+1;
			while (*result == '\n' || *result == '\r') result++;
		}
		
		// Search for the start of a comment, if we don't have one already.
		while (*c != '\0' && result == nullptr) {
			if (*c == '/' && (*(c+1) == '/' || *(c+1) == '*')) {
				result = (char*)(c+2);
				*ref_state = *(c + 1) == '*';
			}
			c++;
		}

		// Find the end of this comment line.
		c = result;
		while (c != nullptr && *c != '\0' && *c != '\n' && *c != '\r') {
			if (*ref_state && *c == '*' && *(c+1) == '/') {
				*ref_state = false;
				break;
			}
			c++;
		}
		*ref_end = c;

		return result;
	};

	// This function checks if the line is relevant for our metadata
	const char *(*is_relevant)(const char *start, const char *end) = [](const char *start, const char *end) {
		const char *c = start;
		while (c != end && (*c == ' ' || *c == '\t')) c++;

		return end - c > 1 && c[0] == '-' && c[1] == '-' 
			? &c[2] 
			: (char*)nullptr;
	};

	void (*trim_str)(const char **ref_start, const char **ref_end) = [] (const char **ref_start, const char **ref_end){
		while (**ref_start   == ' ' || **ref_start   == '\t') (*ref_start)++;
		while (*(*ref_end-1) == ' ' || *(*ref_end-1) == '\t') (*ref_end)--;
	};

	const char *(*index_of)(const char *start, const char *end, char ch) = [](const char *start, const char *end, char ch) {
		while (start != end) {
			if (*start == ch)
				return start;
			start++;
		}
		return (const char*)nullptr;
	};

	bool        in_comment  = false;
	const char *comment_end = nullptr;
	const char *comment     = next_comment(hlsl_text, &comment_end, &in_comment);
	while (comment) {
		comment = is_relevant(comment, comment_end);
		if (comment) {
			const char *tag_str   = index_of(comment, comment_end, ':');
			const char *value_str = index_of(comment, comment_end, '=');

			const char *name_start = comment;
			const char *name_end   = tag_str?tag_str:(value_str?value_str:comment_end);
			trim_str(&name_start, &name_end);
			char name[32];
			int64_t ct = name_end - name_start;
			memcpy(name, name_start, mini(sizeof(name), ct));
			name[ct] = '\0';

			char tag[64]; tag[0] = '\0';
			if (tag_str) {
				const char *tag_start = tag_str + 1;
				const char *tag_end   = value_str ? value_str : comment_end;
				trim_str(&tag_start, &tag_end);
				ct = maxi(0, tag_end - tag_start);
				memcpy(tag, tag_start, mini(sizeof(tag), ct));
				tag[ct] = '\0';
			}

			char value[512]; value[0] = '\0';
			if (value_str) {
				const char *value_start = value_str + 1;
				const char *value_end   = comment_end;
				trim_str(&value_start, &value_end);
				ct = maxi(0, value_end - value_start);
				memcpy(value, value_start, mini(sizeof(value), ct));
				value[ct] = '\0';
			}

			sksc_meta_item_t item = {};
			sksc_line_col(hlsl_text, comment, &item.row, &item.col);
			strncpy(item.name,  name,  sizeof(item.name));
			strncpy(item.tag,   tag,   sizeof(item.tag));
			strncpy(item.value, value, sizeof(item.value));
			items.add(item);

			if (tag[0] == '\0' && value[0] == '\0') {
				sksc_log_at(log_level_warn, item.row, item.col, "Shader var data for '%s' has no tag or value, missing a ':' or '='?", name);
			}
		}
		comment = next_comment(hlsl_text, &comment_end, &in_comment);
	}
	return items;
}

///////////////////////////////////////////

void sksc_meta_assign_defaults(array_t<sksc_meta_item_t> items, sksc_shader_meta_t *ref_meta) {
	int32_t(*count_ch)(const char *str, char ch) = [](const char *str, char ch) {
		const char *c      = str;
		int32_t     result = 0;
		while (*c != '\0') {
			if (*c == ch) result++;
			c++;
		}
		return result;
	};

	for (size_t i = 0; i < items.count; i++) {
		sksc_meta_item_t*     item  = &items[i];
		sksc_shader_buffer_t* buff  = ref_meta->global_buffer_id == -1 ? nullptr : &ref_meta->buffers[ref_meta->global_buffer_id];
		int32_t               found = 0;
		for (size_t v = 0; buff && v < buff->var_count; v++) {
			if (strcmp(buff->vars[v].name, item->name) != 0) continue;
			
			found += 1;
			strncpy(buff->vars[v].extra, item->tag, sizeof(buff->vars[v].extra));

			if (item->value[0] == '\0') break;

			int32_t commas = count_ch(item->value, ',');

			if (buff->vars[v].type == sksc_shader_var_none) {
				sksc_log_at(log_level_warn, item->row, item->col, "Can't set default for --%s, unimplemented type", item->name);
			} else if (commas + 1 != buff->vars[v].type_count) {
				sksc_log_at(log_level_warn, item->row, item->col, "Default value for --%s has an incorrect number of arguments", item->name);
			} else {
				if (buff->defaults == nullptr) {
					buff->defaults = malloc(buff->size);
					memset(buff->defaults, 0, buff->size);
				}
				uint8_t *write_at = ((uint8_t *)buff->defaults) + buff->vars[v].offset;

				char *start = item->value;
				char *end   = strchr(start, ',');
				char  param[64];
				for (size_t c = 0; c <= commas; c++) {
					int32_t length = (int32_t)(end == nullptr ? mini(sizeof(param)-1, strlen(item->value)) : end - start);
					memcpy(param, start, mini(sizeof(param), length));
					param[length] = '\0';

					double d = atof(param);

					switch (buff->vars[v].type) {
					case sksc_shader_var_float:  {float    val = (float   )d; memcpy(write_at, &val, sizeof(val)); write_at += sizeof(val); }break;
					case sksc_shader_var_double: {double   val =           d; memcpy(write_at, &val, sizeof(val)); write_at += sizeof(val); }break;
					case sksc_shader_var_int:    {int32_t  val = (int32_t )d; memcpy(write_at, &val, sizeof(val)); write_at += sizeof(val); }break;
					case sksc_shader_var_uint:   {uint32_t val = (uint32_t)d; memcpy(write_at, &val, sizeof(val)); write_at += sizeof(val); }break;
					case sksc_shader_var_uint8:  {uint8_t  val = (uint8_t )d; memcpy(write_at, &val, sizeof(val)); write_at += sizeof(val); }break;
					}

					if (end != nullptr) {
						start = end + 1;
						end   = strchr(start, ',');
					}
				}
			}
			break;
		}

		for (size_t r = 0; r < ref_meta->resource_count; r++) {
			if (strcmp(ref_meta->resources[r].name, item->name) != 0) continue;
			found += 1;

			strncpy(ref_meta->resources[r].tags,  item->tag,   sizeof(ref_meta->resources[r].tags ));
			strncpy(ref_meta->resources[r].value, item->value, sizeof(ref_meta->resources[r].value));
			break;
		}

		if (strcmp(item->name, "name") == 0) {
			found += 1;
			strncpy(ref_meta->name, item->value, sizeof(ref_meta->name));
		}
		
		if (found != 1) {
			sksc_log_at(log_level_warn, item->row, item->col, "Can't find shader var named '%s'", item->name);
		}
	}
}

///////////////////////////////////////////

bool sksc_meta_check_dup_buffers(const sksc_shader_meta_t *ref_meta) {
	for (size_t i = 0; i < ref_meta->buffer_count; i++) {
		for (size_t t = 0; t < ref_meta->buffer_count; t++) {
			if (i == t) continue;
			if (ref_meta->buffers[i].bind.slot == ref_meta->buffers[t].bind.slot &&
			    ref_meta->buffers[i].space     == ref_meta->buffers[t].space) {
				return false;
			}
		}
	}
	return true;
}

///////////////////////////////////////////

void sksc_line_col(const char *from_text, const char *at, int32_t *out_line, int32_t *out_column) {
	if (out_line  ) *out_line   = -1;
	if (out_column) *out_column = -1;

	bool found = false;
	const char *curr = from_text;
	int32_t line = 0, col = 0;
	while (*curr != '\0') {
		if (*curr == '\n') { line++; col = 0; } 
		else if (*curr != '\r') col++;
		if (curr == at) {
			found = true;
			break;
		}
		curr++;
	}

	if (found) {
		if (out_line  ) *out_line   = line+1;
		if (out_column) *out_column = col;
	}
}