#pragma once

#include <assert.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef int portMUX_TYPE;

#define pdFALSE 0
#define pdTRUE  1
#define pdFAIL  0
#define pdPASS  1
#define portMAX_DELAY UINT32_MAX
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))

#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock)  ((void)(lock))
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock)  ((void)(lock))
#define configASSERT(condition)  assert(condition)
