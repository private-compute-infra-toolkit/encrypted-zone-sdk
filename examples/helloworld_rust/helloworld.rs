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

use ez_isolate_bridge_sdk::{
    create_isolate_server, Greeter, GreeterIsolateRpcService, HelloRequestStream,
};
use greeter_proto::helloworld::{HelloReply, HelloRequest};
use std::sync::Arc;
use tokio::sync::mpsc;
use tokio_stream::wrappers::ReceiverStream;
use tokio_stream::StreamExt;
use tonic::{Request, Response, Status};

const CHANNEL_SIZE: usize = 128;

#[derive(Default)]
struct GreeterImpl;

#[tonic::async_trait]
impl Greeter for GreeterImpl {
    async fn say_hello(
        &self,
        request: Request<HelloRequest>,
    ) -> Result<Response<HelloReply>, Status> {
        println!("Greeter service received a request: {:?}", request);
        let reply = HelloReply { message: format!("Hello {}", request.into_inner().name) };
        Ok(Response::new(reply))
    }

    type StreamingSayHelloStream = ReceiverStream<Result<HelloReply, Status>>;
    async fn streaming_say_hello<T: HelloRequestStream>(
        &self,
        request_stream: Request<T>,
    ) -> Result<Response<Self::StreamingSayHelloStream>, Status> {
        println!("Greeter service received a stream request");
        let (tx, rx) = mpsc::channel(CHANNEL_SIZE);
        tokio::spawn(async move {
            let mut stream = request_stream.into_inner();
            while let Some(request_result) = stream.next().await {
                let request = request_result.unwrap();
                let _ =
                    tx.send(Ok(HelloReply { message: format!("Hello {}", request.name) })).await;
            }
        });
        Ok(Response::new(ReceiverStream::new(rx)))
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let greeter_logic = Arc::new(GreeterImpl);
    let server = create_isolate_server! {
        GreeterIsolateRpcService => greeter_logic
    };

    println!("Starting Greeter Isolate server...");
    server.start(None).await;

    println!("Server shut down gracefully.");
    Ok(())
}
