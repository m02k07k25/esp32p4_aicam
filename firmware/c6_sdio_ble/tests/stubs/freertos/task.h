#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *argument);

BaseType_t xTaskCreate(TaskFunction_t task,
                       const char *name,
                       uint32_t stack_depth,
                       void *argument,
                       UBaseType_t priority,
                       TaskHandle_t *created_task);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks_to_delay);
