#ifndef PHP_APM_H
#define PHP_APM_H

extern zend_module_entry apm_module_entry;
#define phpext_apm_ptr &apm_module_entry

#define PHP_APM_VERSION "0.1.0"

#ifdef ZTS
#include "TSRM.h"
#endif

ZEND_BEGIN_MODULE_GLOBALS(apm)
    zend_bool enabled;
    zend_string *log_file;
    FILE *log_fp;
    uint64_t request_start;
    uint64_t cpu_user_start;
    uint64_t cpu_sys_start;
    HashTable call_times;
    zend_error_cb original_error_cb;
ZEND_END_MODULE_GLOBALS(apm)

ZEND_EXTERN_MODULE_GLOBALS(apm)

#endif /* PHP_APM_H */
