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

const PUBLIC_API_HOST_ENV_VAR: &str = "EZ_PUBLIC_API_HOST";
const PUBLIC_API_PORT_ENV_VAR: &str = "EZ_PUBLIC_API_PORT";
// see enforcer/public_api/src/lib.rs
const EZ_PUBLIC_API_RESPONSE_CHANNEL_SIZE: usize = 128;

#[derive(Parser, Debug)]
#[command(version, about)]
struct ClientArgs {
    /// Total number of RPC requests that should be sent to the summation isolate server.
    #[arg(short = 'n', long, default_value_t = 1)]
    total_num_rpcs: i32,
    /// Number of concurrent tasks sending RPCs sequentially. total_num_rpcs will be divided
    /// evenly across these tasks.  If requests are not exactly divisible, some tasks will have
    /// one more RPC to send than others.
    #[arg(short = 't', long, default_value_t = 1)]
    concurrent_tasks: i32,
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

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let client_args = ClientArgs::parse();
    let host = env::var(PUBLIC_API_HOST_ENV_VAR).unwrap_or(client_args.server_address);
    let port = env::var(PUBLIC_API_PORT_ENV_VAR).unwrap_or(client_args.port.to_string());
    // 3 retries with 1s as base delay
    let retry_strategy = ExponentialBackoff::from_millis(1000).take(3);
    let client = Retry::spawn(retry_strategy, move || {
        ez_api_retryable_connect(format!("http://{}:{}", host, port))
    })
    .await?;

    let requests_per_task = client_args.total_num_rpcs / client_args.concurrent_tasks;
    let remainder = client_args.total_num_rpcs % client_args.concurrent_tasks;

    let mut task_handles = vec![];
    for i in 0..client_args.concurrent_tasks {
        let client_clone = client.clone();
        let service_name = client_args.service_name.clone();
        let method_name = client_args.method_name.clone();
        let requests_for_this_task =
            if i < remainder { requests_per_task + 1 } else { requests_per_task };
        let handle = tokio::spawn(async move {
            if client_args.send_streaming {
                send_requests_stream(
                    client_clone,
                    i,
                    requests_for_this_task,
                    service_name,
                    method_name,
                )
                .await
            } else {
                send_requests(client_clone, i, requests_for_this_task, service_name, method_name)
                    .await
            }
        });
        task_handles.push(handle);
    }

    for handle in task_handles {
        assert!(handle.await.unwrap().is_ok());
    }
    Ok(())
}

async fn send_requests(
    mut client: EzPublicApiClient<Channel>,
    task_number: i32,
    number_of_requests: i32,
    service_name: String,
    method_name: String,
) -> Result<(), anyhow::Error> {
    for i in 0..number_of_requests {
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
                0 => println!("Status was explicitly set OK"),
                other => {
                    println!("raw response: {response:?}");
                    anyhow::bail!("RPC error, Status was {other:?}");
                }
            }
        }
        let public_output_bytes = prost::bytes::Bytes::from(response.public_output);
        let parsed_response = IntegerSequenceResponse::decode(public_output_bytes).unwrap();
        println!("Parsed response {task_number}#{i}: {parsed_response:?}");
        assert_eq!(parsed_response.sequence_sum, get_expected_sum_result(sum_start, sum_end));
    }
    Ok(())
}

async fn send_requests_stream(
    mut client: EzPublicApiClient<Channel>,
    task_number: i32,
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
    let mut i = 0;
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
        println!("Parsed response {task_number}#{i}: {parsed_response:?}");
        i += 1;
        assert_eq!(parsed_response.sequence_sum, get_expected_sum_result(sum_start, sum_end));
    }
    Ok(())
}
