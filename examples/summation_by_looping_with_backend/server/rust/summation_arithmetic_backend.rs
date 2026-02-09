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

use arithmetic_backend::{
    create_isolate_server, AddRequestStream, ArithmeticBackend, ArithmeticBackendIsolateRpcService,
};
use arithmetic_backend_proto::arithmetic_backend::{AddRequest, AddResponse};
use std::sync::Arc;
use tokio::sync::mpsc;
use tokio_stream::wrappers::ReceiverStream;
use tokio_stream::StreamExt;
use tonic::{Request, Response, Status};

const CHANNEL_SIZE: usize = 128;

#[derive(Default)]
struct ArithmeticBackendImpl;

impl ArithmeticBackendImpl {
    fn new() -> Self {
        Self
    }
}

#[tonic::async_trait]
impl ArithmeticBackend for ArithmeticBackendImpl {
    async fn add(&self, request: Request<AddRequest>) -> Result<Response<AddResponse>, Status> {
        let inner_request = request.into_inner();
        let reply = AddResponse { sum: inner_request.left + inner_request.right };
        Ok(Response::new(reply))
    }

    type StreamingAddStream = ReceiverStream<Result<AddResponse, Status>>;
    async fn streaming_add<T: AddRequestStream>(
        &self,
        request_stream: Request<T>,
    ) -> Result<Response<Self::StreamingAddStream>, Status> {
        let (tx, rx) = mpsc::channel(CHANNEL_SIZE);
        tokio::spawn(async move {
            let mut stream = request_stream.into_inner();
            while let Some(request_result) = stream.next().await {
                let request = request_result.unwrap();
                let _ = tx.send(Ok(AddResponse { sum: request.left + request.right })).await;
            }
        });
        Ok(Response::new(ReceiverStream::new(rx)))
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let arithmetic_backend_logic = Arc::new(ArithmeticBackendImpl::new());
    let server = create_isolate_server! {
        ArithmeticBackendIsolateRpcService => arithmetic_backend_logic
    };

    // You can also use create_isolate_server_with_resp_scope to create a server with a specific data scope.
    // let server = create_isolate_server_with_resp_scope! {
    //     ArithmeticBackendIsolateRpcService => arithmetic_backend_logic => DataScopeType::Public
    // };

    println!("Starting Arithmetic Backend Isolate server...");
    server.start(None).await;

    println!("Server shut down gracefully.");
    Ok(())
}
