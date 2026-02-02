#ifndef PHP_APM_H
#define PHP_APM_H

#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>
#include "php.h"

extern zend_module_entry apm_module_entry;
#define phpext_apm_ptr &apm_module_entry

#define PHP_APM_VERSION "0.2.0"
#define APM_MAX_STACK_DEPTH 64

#ifdef ZTS
#include "TSRM.h"
#endif

typedef struct apm_sample_entry {
    uint64_t wall_time_ns;
    uint64_t cpu_time_ns;
    uint32_t depth;
    uintptr_t frames[APM_MAX_STACK_DEPTH];
} apm_sample_entry;

typedef struct apm_cpu_point {
    uint64_t wall_time_ns;
    uint64_t cpu_time_ns;
} apm_cpu_point;

typedef struct apm_ring_buffer {
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    uint32_t dropped;
    apm_sample_entry *samples;
    apm_cpu_point *cpu_points;
} apm_ring_buffer;

ZEND_BEGIN_MODULE_GLOBALS(apm)
    zend_bool enabled;
    zend_bool enable_sampling;
    zend_bool enable_function_timing;
    zend_long sample_interval_us;
    zend_long buffer_size;
    uint64_t request_start_wall;
    uint64_t request_start_cpu;
    uint64_t request_end_wall;
    uint64_t request_end_cpu;
    uint64_t request_memory_start;
    uint64_t request_memory_peak;
    uint32_t error_count;
    uint32_t exception_count;
    zend_error_cb original_error_cb;
    void (*original_exception_hook)(zend_object *ex);
    HashTable call_times;
    HashTable func_metrics;
    apm_ring_buffer ring;
    timer_t sampler_timer;
    struct sigaction old_sigaction;
    zend_bool sampler_active;
} zend_apm_globals;

ZEND_EXTERN_MODULE_GLOBALS(apm)

#endif /* PHP_APM_H */
