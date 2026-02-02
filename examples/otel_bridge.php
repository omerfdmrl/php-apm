<?php

declare(strict_types=1);

use OpenTelemetry\API\Globals;
use OpenTelemetry\API\Trace\SpanKind;
use OpenTelemetry\API\Trace\StatusCode;
use OpenTelemetry\SDK\Trace\TracerProvider;
use OpenTelemetry\SDK\Resource\ResourceInfo;
use OpenTelemetry\SDK\Trace\SpanExporter\OtlpHttpExporter;
use OpenTelemetry\SDK\Trace\SpanProcessor\SimpleSpanProcessor;

require __DIR__ . '/vendor/autoload.php';

$resource = ResourceInfo::create(['service.name' => 'php-apm-service']);
$exporter = new OtlpHttpExporter('http://localhost:4318/v1/traces');
$processor = new SimpleSpanProcessor($exporter);
$tracerProvider = new TracerProvider($processor, $resource);

Globals::setTracerProvider($tracerProvider);
$tracer = Globals::tracerProvider()->getTracer('php-apm');

$span = $tracer->spanBuilder('http.request')
    ->setSpanKind(SpanKind::KIND_SERVER)
    ->startSpan();

$scope = $span->activate();

try {
    // Your application logic here
} catch (Throwable $e) {
    $span->recordException($e);
    $span->setStatus(StatusCode::STATUS_ERROR, $e->getMessage());
} finally {
    $metrics = apm_get_request_metrics();
    $samples = apm_flush_samples();
    $functionMetrics = apm_flush_function_metrics();

    // Map metrics/samples into OTel metrics or profiling exporters.
    // Example: emit cpu/wall delta and memory peak as custom metrics.

    $scope->detach();
    $span->end();
    $tracerProvider->shutdown();
}
