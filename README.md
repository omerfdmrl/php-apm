# PHP APM (C extension)

A minimal PHP 8 APM extension implemented in C using the Zend Observer API. It logs request metadata, errors, traces, and CPU time. The extension is designed for Linux and PHP 8.

## Features

- Request metadata (method/URI)
- Error logging via `zend_error_cb`
- Function call tracing via Zend Observer API
- CPU time and wall time tracking
- Basic Zend and Opcache status markers

## Build and install

```sh
phpize
./configure --enable-apm
make
sudo make install
```

Add to `php.ini`:

```ini
extension=apm
apm.enabled=1
apm.log_file=/var/log/php-apm.log
```

## Notes

- Opcache status is noted as a placeholder in the log. For full details, call `opcache_get_status()` from userland and forward it to your APM backend.
- The extension logs to the configured file or falls back to `php_error_docref` when no file is set.
