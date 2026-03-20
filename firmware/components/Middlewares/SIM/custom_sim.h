#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t custom_sim_start(int core_id, uint32_t stack_size, int priority);
esp_err_t custom_sim_stop(uint32_t timeout_ms);
bool custom_sim_is_running(void);
esp_err_t custom_sim_set_bitmap(const uint8_t* bitmap, size_t len);

#ifdef __cplusplus
}
#endif
