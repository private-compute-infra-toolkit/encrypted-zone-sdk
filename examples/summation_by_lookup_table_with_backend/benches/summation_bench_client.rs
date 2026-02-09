// Copyright 2025 Google LLC
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

use clap::Parser;
use criterion::{Bencher, BenchmarkId, Criterion, SamplingMode, Throughput};
use enforcer_proto::enforcer::v1::ez_public_api_client::EzPublicApiClient;
use prost::Message;
use std::env;
use summation_client_lib::{
    create_call_request_for_summation, ez_api_retryable_connect, get_expected_sum_result,
};
use summation_proto::summation::IntegerSequenceResponse;
use tokio_retry::strategy::ExponentialBackoff;
use tokio_retry::Retry;
use tonic::transport::Channel;

const PUBLIC_API_PORT_ENV_VAR: &str = "EZ_PUBLIC_API_PORT";
// see enforcer/public_api/src/lib.rs
const EZ_PUBLIC_API_RESPONSE_CHANNEL_SIZE: usize = 128;

#[derive(Parser, Debug)]
#[command(version, about)]
struct ClientArgs {
    /// Comma separated list of integers determining the number of concurrent tasks sending RPCs
    /// sequentially in concurrent mode. Each of these integers will represent a separate
    /// benchmark in the benchmark group. This will help benchmark EZ under concurrent load.
    #[arg(short = 't', long, default_value_t = String::from("1,10,100,1000"))]
    concurrency_levels: String,
    /// Number of RPCs sent by each concurrent task one after another. This will
    /// help benchmark EZ under concurrent load.
    #[arg(short = 'c', long, default_value_t = 1)]
    rpcs_per_concurrent_task: i32,
    /// Location where benchmarking results will be stored. This will be used to compare
    /// results from previous iteration. Note: Discard the concurrent load comparison if
    /// you have changed [concurrent_tasks] or [rpcs_per_concurrent_task] from previous run.
    #[arg(short = 'r', long, default_value = "/tmp/bench_resuts/")]
    results_path: String,
    /// IP address of server (default is localhost). Port is set via the EZ_PUBLIC_API_PORT
    /// environment variable.
    #[arg(short = 's', long, default_value = "[::1]")]
    server_address: String,
    /// Port to connect on (overridden by EZ_PUBLIC_API_PORT env variable if set)
    #[arg(short = 'p', long, default_value_t = 53459)]
    port: u64,
    #[arg(short = 'b', long, default_value_t = false)]
    send_streaming: bool,
    #[arg(short = 's', long, default_value = "SimpleAdd")]
    service_name: String,
    #[arg(short = 's', long, default_value = "IntegerSequence")]
    method_name: String,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let client_args = ClientArgs::parse();
    let port = env::var(PUBLIC_API_PORT_ENV_VAR).unwrap_or(client_args.port.to_string());
    // 3 retries with 1s as base delay
    let retry_strategy = ExponentialBackoff::from_millis(1000).take(3);
    let runtime = tokio::runtime::Runtime::new()?;
    let client = runtime.block_on(async {
        Retry::spawn(retry_strategy, move || {
            ez_api_retryable_connect(format!("http://{}:{}", client_args.server_address, port))
        })
        .await
    })?;
    let concurrent_tasks_list = parse_concurrent_tasks_list(client_args.concurrency_levels);

    // To understand how Criterion determines iterations from samples:
    // See: https://bheisler.github.io/criterion.rs/book/analysis.html#measurement

    let mut criterion = Criterion::default()
        .output_directory(std::path::Path::new(&client_args.results_path))
        .with_plots();
    let mut group = criterion.benchmark_group("ez_summation_benchmark");
    group.sampling_mode(SamplingMode::Flat); // So that all samples have same number of iterations

    for concurrent_tasks in concurrent_tasks_list {
        let concurrent_request_sender_params = ConcurrentRequestSenderParams {
            concurrent_tasks,
            rpcs_per_concurrent_task: client_args.rpcs_per_concurrent_task,
        };

        group.throughput(Throughput::Elements(concurrent_tasks.try_into().unwrap()));
        group.bench_with_input(
            BenchmarkId::from_parameter(concurrent_request_sender_params),
            &concurrent_request_sender_params,
            |b: &mut Bencher, concurrent_request_sender_params: &ConcurrentRequestSenderParams| {
                b.to_async(tokio::runtime::Runtime::new().unwrap()).iter(|| async {
                    let client_clone = client.clone();
                    let service_name_clone = client_args.service_name.clone();
                    let method_name_clone = client_args.method_name.clone();
                    send_requests_concurrently(
                        client_clone,
                        concurrent_request_sender_params.concurrent_tasks,
                        client_args.rpcs_per_concurrent_task,
                        client_args.send_streaming,
                        service_name_clone,
                        method_name_clone,
                    )
                    .await;
                });
            },
        );
    }
    group.finish();
    criterion.final_summary();
    Ok(())
}

async fn send_requests_concurrently(
    client: EzPublicApiClient<Channel>,
    concurrent_tasks: i32,
    requests_per_task: i32,
    send_streaming: bool,
    service_name: String,
    method_name: String,
) {
    let mut task_handles = vec![];
    for _ in 0..concurrent_tasks {
        let client_clone = client.clone();
        let service_name_clone = service_name.clone();
        let method_name_clone = method_name.clone();
        let handle = tokio::spawn(async move {
            if send_streaming {
                send_requests_stream(
                    client_clone,
                    requests_per_task,
                    service_name_clone,
                    method_name_clone,
                )
                .await
            } else {
                send_requests(
                    client_clone,
                    requests_per_task,
                    service_name_clone,
                    method_name_clone,
                )
                .await
            }
        });
        task_handles.push(handle);
    }
    for handle in task_handles {
        assert!(handle.await.unwrap().is_ok());
    }
}

async fn send_requests(
    mut client: EzPublicApiClient<Channel>,
    number_of_requests: i32,
    service_name: String,
    method_name: String,
) -> Result<(), anyhow::Error> {
    for _ in 0..number_of_requests {
        let sum_start = 1;
        let sum_end = 100;
        let response = client
            .call(create_call_request_for_summation(
                sum_start,
                sum_end,
                service_name.clone(),
                method_name.clone(),
            ))
            .await
            .unwrap()
            .into_inner();
        // If no status is set, the assumption is no errors occurred
        if response.status.is_some() {
            match response.status.as_ref().unwrap().code {
                0 => {}
                other => {
                    println!("error raw response: {response:?}");
                    anyhow::bail!("RPC error, Status was {other:?}");
                }
            }
        }
        let public_output_bytes = prost::bytes::Bytes::from(response.public_output);
        let parsed_response = IntegerSequenceResponse::decode(public_output_bytes).unwrap();
        assert_eq!(parsed_response.sequence_sum, get_expected_sum_result(sum_start, sum_end));
    }
    Ok(())
}

async fn send_requests_stream(
    mut client: EzPublicApiClient<Channel>,
    number_of_requests: i32,
    service_name: String,
    method_name: String,
) -> Result<(), anyhow::Error> {
    let (client_tx, client_rx) = tokio::sync::mpsc::channel(EZ_PUBLIC_API_RESPONSE_CHANNEL_SIZE);
    let request_stream = tokio_stream::wrappers::ReceiverStream::new(client_rx);
    let mut response_stream = client.stream_call(request_stream).await.unwrap().into_inner();
    let sum_start = 1;
    let sum_end = 100;
    tokio::spawn(async move {
        for _ in 0..number_of_requests {
            let client_tx_clone = client_tx.clone();
            let service_name_clone = service_name.clone();
            let method_name_clone = method_name.clone();
            tokio::spawn(async move {
                let _ = client_tx_clone
                    .send(create_call_request_for_summation(
                        sum_start,
                        sum_end,
                        service_name_clone,
                        method_name_clone,
                    ))
                    .await;
            });
        }
    });
    while let Some(response) = response_stream.message().await.unwrap() {
        // If no status is set, the assumption is no errors occurred
        if response.status.is_some() {
            match response.status.as_ref().unwrap().code {
                0 => println!("Status was explicitly set OK"),
                other => {
                    println!("raw response: {response:?}");
                    anyhow::bail!("RPC error, Status was {other:?}");
                }
            }
        }
        let public_output_bytes = prost::bytes::Bytes::from(response.public_output);
        let parsed_response = IntegerSequenceResponse::decode(public_output_bytes).unwrap();
        assert_eq!(parsed_response.sequence_sum, get_expected_sum_result(sum_start, sum_end));
    }
    Ok(())
}

fn parse_concurrent_tasks_list(concurrency_levels: String) -> Vec<i32> {
    let mut result = vec![];
    let parts: Vec<&str> = concurrency_levels.split(',').collect();
    for part in parts {
        result.push(part.parse::<i32>().unwrap());
    }
    result
}

#[derive(Copy, Clone)]
struct ConcurrentRequestSenderParams {
    pub concurrent_tasks: i32,
    pub rpcs_per_concurrent_task: i32,
}

// Required by Bencher to distinguish runs with different params
impl std::fmt::Display for ConcurrentRequestSenderParams {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "[ConcurrentTasks/RpcsPerTask]:[{}/{}]",
            self.concurrent_tasks, self.rpcs_per_concurrent_task
        )
    }
}
