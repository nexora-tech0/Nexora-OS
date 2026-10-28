#ifndef NEXORA_BROWSER_JSENGINE_H
#define NEXORA_BROWSER_JSENGINE_H

#include <stddef.h>
#include <stdint.h>

/* Declaration-only contract; no runtime implementation is linked yet. */
typedef struct JsEngine JsEngine;

typedef enum JsEngineStatus {
    JSENGINE_OK = 0,
    JSENGINE_INVALID_ARGUMENT,
    JSENGINE_OUT_OF_MEMORY,
    JSENGINE_SOURCE_LIMIT,
    JSENGINE_TIME_LIMIT,
    JSENGINE_SCRIPT_EXCEPTION,
    JSENGINE_RESULT_LIMIT
} JsEngineStatus;

typedef struct JsEngineConfig {
    size_t MemoryLimitBytes;
    size_t StackLimitBytes;
    size_t MaxSourceBytes;
    uint64_t TimeLimitMicroseconds;
} JsEngineConfig;

enum {
    JSENGINE_DEFAULT_MEMORY_LIMIT = 32 * 1024 * 1024,
    JSENGINE_DEFAULT_STACK_LIMIT = 512 * 1024,
    JSENGINE_DEFAULT_MAX_SOURCE_BYTES = 256 * 1024,
    JSENGINE_DEFAULT_TIME_LIMIT_US = 1000000
};

JsEngine *JsEngineCreate(const JsEngineConfig *Config, JsEngineStatus *Status);
JsEngineStatus JsEngineEvaluate(JsEngine *Engine, const char *Source, size_t SourceLength,
                                char *Result, size_t ResultCapacity, size_t *ResultLength,
                                char *Error, size_t ErrorCapacity);
void JsEngineDestroy(JsEngine *Engine);

#endif
