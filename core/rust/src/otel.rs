// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use opentelemetry::global;
use opentelemetry_otlp::WithExportConfig;

/// Initializes the global OpenTelemetry MeterProvider pointing to the enforcer Unix Domain Socket.
pub fn init_metrics() {
    let endpoint = std::env::var("OTEL_EXPORTER_OTLP_METRICS_ENDPOINT")
        .unwrap_or_else(|_| "unix:///enforcer-isolate-shared/otlp-metrics.sock".to_string());

    if !endpoint.starts_with("unix://") {
        log::info!(
            "OTel metrics endpoint does not start with unix://, skipping UDS configuration."
        );
        return;
    }

    log::info!("Initializing SDK metrics exporter targeting UDS: {}", endpoint);

    let exporter_res =
        opentelemetry_otlp::new_exporter().tonic().with_endpoint(endpoint).build_metrics_exporter(
            Box::new(opentelemetry_sdk::metrics::reader::DefaultAggregationSelector::new()),
            Box::new(opentelemetry_sdk::metrics::reader::DefaultTemporalitySelector::new()),
        );

    let exporter = match exporter_res {
        Ok(exp) => exp,
        Err(e) => {
            log::warn!("Skipping OTel metrics initialization: failed to construct exporter: {}", e);
            return;
        }
    };

    let reader = opentelemetry_sdk::metrics::PeriodicReader::builder(
        exporter,
        opentelemetry_sdk::runtime::Tokio,
    )
    .build();

    let provider =
        opentelemetry_sdk::metrics::SdkMeterProvider::builder().with_reader(reader).build();

    global::set_meter_provider(provider);
    log::info!("Global OpenTelemetry MeterProvider configured.");
}
