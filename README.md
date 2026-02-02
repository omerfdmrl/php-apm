# PHP APM (Hybrid C + PHP OpenTelemetry)

This repository provides a production-grade **hybrid** PHP APM agent:

- **Native C extension** for low-level telemetry (CPU, wall time, memory, sampling, errors/exceptions).
- **PHP userland layer** that converts native telemetry to OpenTelemetry traces and metrics.

The extension targets **PHP 8.1+** on Linux and uses **CLOCK_THREAD_CPUTIME_ID** for CPU and **CLOCK_MONOTONIC** for wall time.

## Responsibilities split

### C extension (native agent)
- Request lifecycle hooks (RINIT/RSHUTDOWN)
- Thread CPU time + wall time
- Sampling-based profiling (SIGPROF + timer_create)
- Optional function-level CPU aggregation
- Memory usage (start/peak)
- Error + exception hooks
- Writes data into a fixed-size ring buffer

### PHP layer
- Builds OpenTelemetry spans
- Exports metrics/traces (OTLP)
- Framework integration (Laravel/Symfony)
- Maps metrics to OTel semantic conventions

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
apm.enable_sampling=1
apm.sample_interval_us=10000
apm.enable_function_timing=0
apm.buffer_size=2048
```

## C extension API

The C extension exposes pull-style functions to the PHP layer:

- `apm_get_request_metrics()` → request wall/cpu time, memory, errors
- `apm_flush_samples()` → sampled stacks for flamegraphs
- `apm_flush_function_metrics()` → optional per-function CPU totals

## Notes

- Sampling is enabled by default; function-level timing is gated to avoid overhead.
- Stack samples are captured in a ring buffer and flushed in userland.
- Flamegraphs can be generated from sampled stacks (folded stack format).
