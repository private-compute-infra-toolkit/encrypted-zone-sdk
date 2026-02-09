/*
 * Copyright 2025 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

use arithmetic_backend::ArithmeticBackendIsolateStub;
use arithmetic_backend_proto::arithmetic_backend::AddRequest;
use std::sync::Arc;
use summation::{
    create_isolate_server_with_client, with_sdk_client, IntegerSequenceRequestStream,
    IsolateEzBridgeSdkClient, SimpleAdd, SimpleAddIsolateRpcService,
};
use summation_proto::summation::{IntegerSequenceRequest, IntegerSequenceResponse};
use tokio::sync::mpsc::channel;
use tokio_stream::{wrappers::ReceiverStream, StreamExt};
use tonic::{Request, Response, Status};

const CHANNEL_SIZE: usize = 128;

#[with_sdk_client]
#[derive(Default)]
struct SimpleAddImpl {}

impl SimpleAddImpl {
    async fn process_sequence(
        client: Arc<IsolateEzBridgeSdkClient>,
        inner_request: IntegerSequenceRequest,
    ) -> Result<IntegerSequenceResponse, Status> {
        let start_at = inner_request.start_at;
        let end_at = inner_request.end_at;
        let expected_result = inner_request.expected_result.unwrap_or_default();
        println!(
            "SimpleAdd.IntegerSequence inputs: start_at = {}, end_at = {}, expected_result = {}",
            start_at, end_at, expected_result
        );

        let arithmetic_client =
            ArithmeticBackendIsolateStub::new("playground_example".to_string(), client);

        let mut result: i64 = 0;
        for i in start_at..=end_at {
            let add_request = AddRequest { left: result, right: i };
            let add_response_result = arithmetic_client.add(Request::new(add_request)).await;

            let add_response = match add_response_result {
                Ok(response) => response.into_inner(),
                Err(status) => {
                    return Err(status);
                }
            };
            println!("Add response: {:?}", add_response);

            result = add_response.sum;
        }

        let response = IntegerSequenceResponse { sequence_sum: result };

        if expected_result != 0 && expected_result != result {
            return Err(Status::internal("Expected result mismatch"));
        }

        Ok(response)
    }

    async fn process_stream_sequence(
        client: Arc<IsolateEzBridgeSdkClient>,
        inner_request: IntegerSequenceRequest,
    ) -> Result<IntegerSequenceResponse, Status> {
        let start_at = inner_request.start_at;
        let end_at = inner_request.end_at;
        let expected_result = inner_request.expected_result.unwrap_or_default();
        println!(
            "SimpleAdd.IntegerSequence inputs: start_at = {}, end_at = {}, expected_result = {}",
            start_at, end_at, expected_result
        );

        let arithmetic_client =
            ArithmeticBackendIsolateStub::new("playground_example".to_string(), client);

        let (tx, rx) = channel(CHANNEL_SIZE);
        let request_stream = ReceiverStream::new(rx);

        let mut response_stream =
            arithmetic_client.streaming_add(Request::new(request_stream)).await?.into_inner();

        let mut result: i64 = 0;
        for i in start_at..=end_at {
            let add_request = AddRequest { left: result, right: i };
            tx.send(add_request).await.map_err(|e| Status::internal(e.to_string()))?;
            let add_response_result = response_stream.next().await;

            let add_response = match add_response_result {
                Some(response) => response,
                None => {
                    return Err(Status::internal("No response from backend"));
                }
            };
            println!("Add response: {:?}", add_response);

            result = add_response?.sum;
        }

        let response = IntegerSequenceResponse { sequence_sum: result };

        if expected_result != 0 && expected_result != result {
            return Err(Status::internal("Expected result mismatch"));
        }

        Ok(response)
    }
}

#[tonic::async_trait]
impl SimpleAdd for SimpleAddImpl {
    async fn integer_sequence(
        &self,
        request: Request<IntegerSequenceRequest>,
    ) -> Result<Response<IntegerSequenceResponse>, Status> {
        println!("Summation service received a request: {:?}", request);

        let inner_request = request.into_inner();
        let response = Self::process_sequence(self.get_client(), inner_request).await?;
        Ok(Response::new(response))
    }

    type StreamingIntegerSequenceStream = ReceiverStream<Result<IntegerSequenceResponse, Status>>;
    async fn streaming_integer_sequence<T: IntegerSequenceRequestStream>(
        &self,
        request: Request<T>,
    ) -> Result<Response<Self::StreamingIntegerSequenceStream>, Status> {
        println!("Summation service received a streaming request");

        let mut input_stream = request.into_inner();
        let (tx, rx) = channel(CHANNEL_SIZE);
        let client = self.get_client();

        tokio::spawn(async move {
            while let Some(req_result) = input_stream.next().await {
                let result = match req_result {
                    Ok(req) => Self::process_stream_sequence(client.clone(), req).await,
                    Err(e) => Err(e),
                };

                if tx.send(result).await.is_err() {
                    break;
                }
            }
        });

        Ok(Response::new(ReceiverStream::new(rx)))
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let simple_add_logic = Arc::new(SimpleAddImpl::default());
    let server = create_isolate_server_with_client! {
        SimpleAddIsolateRpcService => simple_add_logic
    };

    // You can also use create_isolate_server_with_resp_scope_and_client to create a server with a specific data scope.
    // let server = create_isolate_server_with_resp_scope_and_client! {
    //     SimpleAddIsolateRpcService => simple_add_logic => DataScopeType::Public
    // };

    println!("Starting Integer Sequence Isolate server...");
    server.start(None).await;

    println!("Server shut down gracefully.");
    Ok(())
}
