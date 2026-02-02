#include "php_apm.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"
#include "Zend/zend_smart_str.h"
#include "ext/standard/info.h"
#include <inttypes.h>
#include <stdarg.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

ZEND_DECLARE_MODULE_GLOBALS(apm)

static uint64_t apm_clock_now(clockid_t clock_id)
{
    struct timespec ts;
    if (clock_gettime(clock_id, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t apm_wall_time_now(void)
{
    return apm_clock_now(CLOCK_MONOTONIC);
}

static uint64_t apm_thread_cpu_now(void)
{
    return apm_clock_now(CLOCK_THREAD_CPUTIME_ID);
}

static void apm_log(const char *format, ...)
{
    va_list args;
    char *buffer = NULL;

    if (!APM_G(enabled)) {
        return;
    }

    va_start(args, format);
    vspprintf(&buffer, 0, format, args);
    va_end(args);

    if (buffer) {
        php_error_docref(NULL, E_NOTICE, "[apm] %s", buffer);
        efree(buffer);
    }
}

static void apm_ring_init(uint32_t capacity)
{
    APM_G(ring).capacity = capacity;
    APM_G(ring).head = 0;
    APM_G(ring).tail = 0;
    APM_G(ring).dropped = 0;
    APM_G(ring).samples = ecalloc(capacity, sizeof(apm_sample_entry));
    APM_G(ring).cpu_points = ecalloc(capacity, sizeof(apm_cpu_point));
}

static void apm_ring_destroy(void)
{
    if (APM_G(ring).samples) {
        efree(APM_G(ring).samples);
        APM_G(ring).samples = NULL;
    }
    if (APM_G(ring).cpu_points) {
        efree(APM_G(ring).cpu_points);
        APM_G(ring).cpu_points = NULL;
    }
    APM_G(ring).capacity = 0;
    APM_G(ring).head = 0;
    APM_G(ring).tail = 0;
    APM_G(ring).dropped = 0;
}

static uint32_t apm_ring_next(uint32_t value, uint32_t capacity)
{
    value++;
    if (value >= capacity) {
        return 0;
    }
    return value;
}

static void apm_ring_push_sample(const apm_sample_entry *entry, const apm_cpu_point *cpu_point)
{
    apm_ring_buffer *ring = &APM_G(ring);
    uint32_t next = apm_ring_next(ring->head, ring->capacity);

    if (next == ring->tail) {
        ring->tail = apm_ring_next(ring->tail, ring->capacity);
        ring->dropped++;
    }

    ring->samples[ring->head] = *entry;
    ring->cpu_points[ring->head] = *cpu_point;
    ring->head = next;
}

static void apm_error_cb(int type, const char *error_filename, const uint32_t error_lineno, zend_string *message)
{
    if (APM_G(enabled)) {
        APM_G(error_count)++;
        apm_log("error type=%d file=%s line=%u message=%s",
            type,
            error_filename ? error_filename : "unknown",
            error_lineno,
            message ? ZSTR_VAL(message) : "unknown");
    }

    if (APM_G(original_error_cb)) {
        APM_G(original_error_cb)(type, error_filename, error_lineno, message);
    }
}

static void apm_exception_hook(zend_object *ex)
{
    if (APM_G(enabled)) {
        APM_G(exception_count)++;
        if (ex && ex->ce) {
            apm_log("exception class=%s", ZSTR_VAL(ex->ce->name));
        }
    }

    if (APM_G(original_exception_hook)) {
        APM_G(original_exception_hook)(ex);
    }
}

static void apm_observer_fcall_begin(zend_execute_data *execute_data)
{
    uint64_t cpu_start = apm_thread_cpu_now();
    uint64_t wall_start = apm_wall_time_now();
    uint64_t payload[2] = {cpu_start, wall_start};

    zend_hash_index_update_mem(&APM_G(call_times), (zend_ulong)execute_data, payload, sizeof(payload));
}

static void apm_observer_fcall_end(zend_execute_data *execute_data, zval *return_value)
{
    uint64_t *payload = zend_hash_index_find_ptr(&APM_G(call_times), (zend_ulong)execute_data);

    if (payload != NULL) {
        uint64_t cpu_delta = apm_thread_cpu_now() - payload[0];
        uint64_t wall_delta = apm_wall_time_now() - payload[1];
        zend_hash_index_del(&APM_G(call_times), (zend_ulong)execute_data);

        if (APM_G(enable_function_timing)) {
            const char *func_name = "{main}";
            if (execute_data && execute_data->func && execute_data->func->common.function_name) {
                func_name = ZSTR_VAL(execute_data->func->common.function_name);
            }

            zval *entry = zend_hash_str_find(&APM_G(func_metrics), func_name, strlen(func_name));
            if (!entry) {
                zval metrics;
                array_init(&metrics);
                add_assoc_long(&metrics, "cpu_ns", (zend_long)cpu_delta);
                add_assoc_long(&metrics, "wall_ns", (zend_long)wall_delta);
                add_assoc_long(&metrics, "calls", 1);
                zend_hash_str_add_new(&APM_G(func_metrics), func_name, strlen(func_name), &metrics);
            } else {
                zval *cpu_val = zend_hash_str_find(Z_ARRVAL_P(entry), "cpu_ns", sizeof("cpu_ns") - 1);
                zval *wall_val = zend_hash_str_find(Z_ARRVAL_P(entry), "wall_ns", sizeof("wall_ns") - 1);
                zval *calls_val = zend_hash_str_find(Z_ARRVAL_P(entry), "calls", sizeof("calls") - 1);
                if (cpu_val) {
                    Z_LVAL_P(cpu_val) += (zend_long)cpu_delta;
                }
                if (wall_val) {
                    Z_LVAL_P(wall_val) += (zend_long)wall_delta;
                }
                if (calls_val) {
                    Z_LVAL_P(calls_val) += 1;
                }
            }
        }
    }

    (void)return_value;
}

static zend_observer_fcall_handlers apm_observer_fcall_register(zend_execute_data *execute_data)
{
    zend_observer_fcall_handlers handlers = {0};

    if (!APM_G(enabled) || !APM_G(enable_function_timing)) {
        return handlers;
    }

    handlers.begin = apm_observer_fcall_begin;
    handlers.end = apm_observer_fcall_end;
    return handlers;
}

static void apm_capture_sample(void)
{
    apm_sample_entry entry;
    apm_cpu_point cpu_point;
    zend_execute_data *execute_data = EG(current_execute_data);
    uint32_t depth = 0;

    entry.wall_time_ns = apm_wall_time_now();
    entry.cpu_time_ns = apm_thread_cpu_now();

    while (execute_data && depth < APM_MAX_STACK_DEPTH) {
        entry.frames[depth++] = (uintptr_t)execute_data->func;
        execute_data = execute_data->prev_execute_data;
    }

    entry.depth = depth;
    cpu_point.wall_time_ns = entry.wall_time_ns;
    cpu_point.cpu_time_ns = entry.cpu_time_ns;

    apm_ring_push_sample(&entry, &cpu_point);
}

static void apm_sampler_signal_handler(int signo)
{
    if (signo != SIGPROF || !APM_G(enabled) || !APM_G(enable_sampling)) {
        return;
    }

    apm_capture_sample();
}

static void apm_sampler_start(void)
{
    struct sigevent sev;
    struct itimerspec its;

    if (!APM_G(enable_sampling) || APM_G(sampler_active)) {
        return;
    }

    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGPROF;

    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &APM_G(sampler_timer)) != 0) {
        apm_log("sampler timer_create failed");
        return;
    }

    memset(&its, 0, sizeof(its));
    its.it_interval.tv_sec = APM_G(sample_interval_us) / 1000000;
    its.it_interval.tv_nsec = (APM_G(sample_interval_us) % 1000000) * 1000;
    its.it_value = its.it_interval;

    if (timer_settime(APM_G(sampler_timer), 0, &its, NULL) != 0) {
        timer_delete(APM_G(sampler_timer));
        apm_log("sampler timer_settime failed");
        return;
    }

    APM_G(sampler_active) = 1;
}

static void apm_sampler_stop(void)
{
    if (!APM_G(sampler_active)) {
        return;
    }

    timer_delete(APM_G(sampler_timer));
    APM_G(sampler_active) = 0;
}

static void apm_install_signal_handler(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = apm_sampler_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGPROF, &sa, &APM_G(old_sigaction)) != 0) {
        apm_log("sigaction install failed");
    }
}

static void apm_restore_signal_handler(void)
{
    sigaction(SIGPROF, &APM_G(old_sigaction), NULL);
}

static void apm_init_globals(zend_apm_globals *apm_globals)
{
    apm_globals->enabled = 1;
    apm_globals->enable_sampling = 1;
    apm_globals->enable_function_timing = 0;
    apm_globals->sample_interval_us = 10000;
    apm_globals->buffer_size = 1024;
    apm_globals->request_start_wall = 0;
    apm_globals->request_start_cpu = 0;
    apm_globals->request_end_wall = 0;
    apm_globals->request_end_cpu = 0;
    apm_globals->request_memory_start = 0;
    apm_globals->request_memory_peak = 0;
    apm_globals->error_count = 0;
    apm_globals->exception_count = 0;
    apm_globals->original_error_cb = NULL;
    apm_globals->original_exception_hook = NULL;
    apm_globals->sampler_active = 0;
    apm_globals->sampler_timer = (timer_t)0;
}

PHP_INI_BEGIN()
    STD_PHP_INI_BOOLEAN("apm.enabled", "1", PHP_INI_ALL, OnUpdateBool, enabled, zend_apm_globals, apm_globals)
    STD_PHP_INI_BOOLEAN("apm.enable_sampling", "1", PHP_INI_ALL, OnUpdateBool, enable_sampling, zend_apm_globals, apm_globals)
    STD_PHP_INI_BOOLEAN("apm.enable_function_timing", "0", PHP_INI_ALL, OnUpdateBool, enable_function_timing, zend_apm_globals, apm_globals)
    STD_PHP_INI_ENTRY("apm.sample_interval_us", "10000", PHP_INI_ALL, OnUpdateLong, sample_interval_us, zend_apm_globals, apm_globals)
    STD_PHP_INI_ENTRY("apm.buffer_size", "1024", PHP_INI_ALL, OnUpdateLong, buffer_size, zend_apm_globals, apm_globals)
PHP_INI_END()

static PHP_FUNCTION(apm_get_request_metrics)
{
    array_init(return_value);
    add_assoc_long(return_value, "wall_start_ns", (zend_long)APM_G(request_start_wall));
    add_assoc_long(return_value, "cpu_start_ns", (zend_long)APM_G(request_start_cpu));
    add_assoc_long(return_value, "wall_end_ns", (zend_long)APM_G(request_end_wall));
    add_assoc_long(return_value, "cpu_end_ns", (zend_long)APM_G(request_end_cpu));
    add_assoc_long(return_value, "memory_start_bytes", (zend_long)APM_G(request_memory_start));
    add_assoc_long(return_value, "memory_peak_bytes", (zend_long)APM_G(request_memory_peak));
    add_assoc_long(return_value, "errors", (zend_long)APM_G(error_count));
    add_assoc_long(return_value, "exceptions", (zend_long)APM_G(exception_count));
    add_assoc_long(return_value, "samples_dropped", (zend_long)APM_G(ring).dropped);
}

static PHP_FUNCTION(apm_flush_samples)
{
    apm_ring_buffer *ring = &APM_G(ring);
    array_init(return_value);

    while (ring->tail != ring->head) {
        apm_sample_entry *entry = &ring->samples[ring->tail];
        apm_cpu_point *cpu_point = &ring->cpu_points[ring->tail];
        zval sample;
        zval frames;

        array_init(&sample);
        add_assoc_long(&sample, "wall_time_ns", (zend_long)cpu_point->wall_time_ns);
        add_assoc_long(&sample, "cpu_time_ns", (zend_long)cpu_point->cpu_time_ns);

        array_init(&frames);
        for (uint32_t i = 0; i < entry->depth; i++) {
            zend_function *func = (zend_function *)entry->frames[i];
            if (func && func->common.function_name) {
                add_next_index_string(&frames, ZSTR_VAL(func->common.function_name));
            } else {
                add_next_index_string(&frames, "{main}");
            }
        }
        add_assoc_zval(&sample, "stack", &frames);
        add_next_index_zval(return_value, &sample);

        ring->tail = apm_ring_next(ring->tail, ring->capacity);
    }
}

static PHP_FUNCTION(apm_flush_function_metrics)
{
    array_init(return_value);
    zend_string *key;
    zval *val;

    ZEND_HASH_FOREACH_STR_KEY_VAL(&APM_G(func_metrics), key, val) {
        if (key && val) {
            add_assoc_zval(return_value, ZSTR_VAL(key), val);
            Z_TRY_ADDREF_P(val);
        }
    } ZEND_HASH_FOREACH_END();

    zend_hash_clean(&APM_G(func_metrics));
}

static const zend_function_entry apm_functions[] = {
    PHP_FE(apm_get_request_metrics, NULL)
    PHP_FE(apm_flush_samples, NULL)
    PHP_FE(apm_flush_function_metrics, NULL)
    PHP_FE_END
};

PHP_MINIT_FUNCTION(apm)
{
    ZEND_INIT_MODULE_GLOBALS(apm, apm_init_globals, NULL);
    REGISTER_INI_ENTRIES();
    APM_G(original_error_cb) = zend_error_cb;
    zend_error_cb = apm_error_cb;

    APM_G(original_exception_hook) = zend_throw_exception_hook;
    zend_throw_exception_hook = apm_exception_hook;

    zend_observer_fcall_register = apm_observer_fcall_register;
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(apm)
{
    zend_throw_exception_hook = APM_G(original_exception_hook);
    zend_error_cb = APM_G(original_error_cb);
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

PHP_RINIT_FUNCTION(apm)
{
#if defined(ZTS) && defined(COMPILE_DL_APM)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif

    if (!APM_G(enabled)) {
        return SUCCESS;
    }

    apm_ring_init((uint32_t)APM_G(buffer_size));
    zend_hash_init(&APM_G(call_times), 64, NULL, NULL, 0);
    zend_hash_init(&APM_G(func_metrics), 64, NULL, ZVAL_PTR_DTOR, 0);

    APM_G(request_start_wall) = apm_wall_time_now();
    APM_G(request_start_cpu) = apm_thread_cpu_now();
    APM_G(request_memory_start) = zend_memory_usage(0);
    APM_G(request_memory_peak) = 0;
    APM_G(error_count) = 0;
    APM_G(exception_count) = 0;

    if (APM_G(enable_sampling)) {
        apm_install_signal_handler();
        apm_sampler_start();
    }

    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(apm)
{
    if (!APM_G(enabled)) {
        return SUCCESS;
    }

    if (APM_G(enable_sampling)) {
        apm_sampler_stop();
        apm_restore_signal_handler();
    }

    APM_G(request_end_wall) = apm_wall_time_now();
    APM_G(request_end_cpu) = apm_thread_cpu_now();
    APM_G(request_memory_peak) = zend_memory_peak_usage(0);

    zend_hash_destroy(&APM_G(call_times));
    zend_hash_destroy(&APM_G(func_metrics));
    apm_ring_destroy();

    return SUCCESS;
}

PHP_MINFO_FUNCTION(apm)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "apm support", "enabled");
    php_info_print_table_row(2, "Version", PHP_APM_VERSION);
    php_info_print_table_row(2, "Sampling", APM_G(enable_sampling) ? "enabled" : "disabled");
    php_info_print_table_end();

    DISPLAY_INI_ENTRIES();
}

zend_module_entry apm_module_entry = {
    STANDARD_MODULE_HEADER,
    "apm",
    apm_functions,
    PHP_MINIT(apm),
    PHP_MSHUTDOWN(apm),
    PHP_RINIT(apm),
    PHP_RSHUTDOWN(apm),
    PHP_MINFO(apm),
    PHP_APM_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_APM
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE();
#endif
ZEND_GET_MODULE(apm)
#endif
