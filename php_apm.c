#include "php.h"
#include "php_apm.h"
#include "Zend/zend_extensions.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"
#include "Zend/zend_smart_str.h"
#include "ext/standard/info.h"
#include <inttypes.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <time.h>

ZEND_DECLARE_MODULE_GLOBALS(apm)

static void php_apm_init_globals(zend_apm_globals *apm_globals)
{
    apm_globals->enabled = 1;
    apm_globals->log_file = NULL;
    apm_globals->log_fp = NULL;
    apm_globals->request_start = 0;
    apm_globals->cpu_user_start = 0;
    apm_globals->cpu_sys_start = 0;
    apm_globals->original_error_cb = NULL;
}

static void apm_log(const char *format, ...)
{
    va_list args;
    char *buffer = NULL;

    if (!APM_G(enabled)) {
        return;
    }

    va_start(args, format);
    if (APM_G(log_fp)) {
        vfprintf(APM_G(log_fp), format, args);
        fprintf(APM_G(log_fp), "\n");
        fflush(APM_G(log_fp));
    } else {
        vspprintf(&buffer, 0, format, args);
        if (buffer) {
            php_error_docref(NULL, E_NOTICE, "[apm] %s", buffer);
            efree(buffer);
        }
    }
    va_end(args);
}

static uint64_t apm_hrtime_now(void)
{
    return zend_hrtime();
}

static uint64_t apm_cpu_time_usec(void)
{
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    return (uint64_t)usage.ru_utime.tv_sec * 1000000ULL + (uint64_t)usage.ru_utime.tv_usec
        + (uint64_t)usage.ru_stime.tv_sec * 1000000ULL + (uint64_t)usage.ru_stime.tv_usec;
}

static void apm_error_cb(int type, const char *error_filename, const uint32_t error_lineno, zend_string *message)
{
    if (APM_G(enabled)) {
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

static void apm_observer_fcall_begin(zend_execute_data *execute_data)
{
    uint64_t start = apm_hrtime_now();
    zend_hash_index_update_mem(&APM_G(call_times), (zend_ulong)execute_data, &start, sizeof(start));
}

static void apm_observer_fcall_end(zend_execute_data *execute_data, zval *return_value)
{
    uint64_t *start_ptr = zend_hash_index_find_ptr(&APM_G(call_times), (zend_ulong)execute_data);
    uint64_t duration = 0;

    if (start_ptr != NULL) {
        duration = apm_hrtime_now() - *start_ptr;
        zend_hash_index_del(&APM_G(call_times), (zend_ulong)execute_data);
    }

    if (APM_G(enabled)) {
        const char *func_name = "{main}";
        if (execute_data && execute_data->func && execute_data->func->common.function_name) {
            func_name = ZSTR_VAL(execute_data->func->common.function_name);
        }
        apm_log("trace function=%s duration_ns=%" PRIu64, func_name, duration);
    }

    (void)return_value;
}

static zend_observer_fcall_handlers apm_observer_fcall_register(zend_execute_data *execute_data)
{
    zend_observer_fcall_handlers handlers = {0};

    if (!APM_G(enabled)) {
        return handlers;
    }

    handlers.begin = apm_observer_fcall_begin;
    handlers.end = apm_observer_fcall_end;
    return handlers;
}

PHP_INI_BEGIN()
    STD_PHP_INI_BOOLEAN("apm.enabled", "1", PHP_INI_ALL, OnUpdateBool, enabled, zend_apm_globals, apm_globals)
    STD_PHP_INI_ENTRY("apm.log_file", "", PHP_INI_ALL, OnUpdateString, log_file, zend_apm_globals, apm_globals)
PHP_INI_END()

PHP_MINIT_FUNCTION(apm)
{
    REGISTER_INI_ENTRIES();

    APM_G(original_error_cb) = zend_error_cb;
    zend_error_cb = apm_error_cb;

    zend_observer_fcall_register = apm_observer_fcall_register;

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(apm)
{
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

    zend_hash_init(&APM_G(call_times), 16, NULL, NULL, 0);

    APM_G(request_start) = apm_hrtime_now();
    APM_G(cpu_user_start) = apm_cpu_time_usec();
    APM_G(cpu_sys_start) = 0;

    if (APM_G(log_file) && ZSTR_LEN(APM_G(log_file)) > 0) {
        APM_G(log_fp) = fopen(ZSTR_VAL(APM_G(log_file)), "a");
        if (!APM_G(log_fp)) {
            php_error_docref(NULL, E_WARNING, "Unable to open apm.log_file: %s", ZSTR_VAL(APM_G(log_file)));
        }
    }

    if (SG(request_info).request_method && SG(request_info).request_uri) {
        apm_log("request method=%s uri=%s", SG(request_info).request_method, SG(request_info).request_uri);
    } else {
        apm_log("request method=cli uri=cli");
    }

    apm_log("zend_status version=%s", PHP_VERSION);
    apm_log("opcache_status note=use_opcache_get_status_in_userland");

    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(apm)
{
    if (!APM_G(enabled)) {
        return SUCCESS;
    }

    uint64_t duration = apm_hrtime_now() - APM_G(request_start);
    uint64_t cpu_total = apm_cpu_time_usec();

    apm_log("request duration_ns=%" PRIu64 " cpu_total_usec=%" PRIu64, duration, cpu_total);

    if (APM_G(log_fp)) {
        fclose(APM_G(log_fp));
        APM_G(log_fp) = NULL;
    }

    zend_hash_destroy(&APM_G(call_times));

    return SUCCESS;
}

PHP_MINFO_FUNCTION(apm)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "apm support", "enabled");
    php_info_print_table_row(2, "Version", PHP_APM_VERSION);
    php_info_print_table_end();

    DISPLAY_INI_ENTRIES();
}

zend_module_entry apm_module_entry = {
    STANDARD_MODULE_HEADER,
    "apm",
    NULL,
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
