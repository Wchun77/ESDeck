#pragma once

#include <stdbool.h>
#include <stddef.h>

void nvs_manager_init(void);

bool nvs_manager_set_str(const char *namespace_name, const char *key, const char *value);
bool nvs_manager_get_str(const char *namespace_name, const char *key, char *out, size_t out_size);