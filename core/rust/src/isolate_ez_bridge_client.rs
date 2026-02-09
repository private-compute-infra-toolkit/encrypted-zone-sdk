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

use enforcer_proto::enforcer::v1::isolate_ez_bridge_client::IsolateEzBridgeClient;
use enforcer_proto::enforcer::v1::{
    InvokeEzRequest, InvokeEzResponse, IsolateState, NotifyIsolateStateRequest,
};
use hyper_util::rt::TokioIo;
use once_cell::sync::Lazy;
use std::env;
use std::error::Error;
use tokio::net::UnixStream;
use tonic::transport::{Channel, Endpoint, Uri};
use tonic::{IntoRequest, IntoStreamingRequest, Status, Streaming};
use tower::service_fn;

static CLIENT_UDS_PATH: Lazy<String> = Lazy::new(|| {
    env::var("CLIENT_UDS_PATH")
        .unwrap_or_else(|_| "/enforcer-isolate-shared/isolate-ez-bridge-uds".to_string())
});

/// A client for interacting with the Isolate EZ Bridge, enabling RPC calls and shared memory.
///
/// This client handles the low-level details of establishing a connection to the bridge
/// via a Unix Domain Socket (UDS) and provides methods for invoking RPCs on the Enforcer.
#[derive(Debug)]
pub struct IsolateEzBridgeSdkClient {
    client: IsolateEzBridgeClient<Channel>,
}

impl IsolateEzBridgeSdkClient {
    /// Establishes the connection to the gRPC server over a Unix Domain Socket.
    ///
    /// This is an async constructor and should generally be called once by the `IsolateRpcServer`
    /// to create a shared client instance.
    ///
    /// # Returns
    ///
    /// Returns a `Result` containing the new `IsolateEzBridgeSdkClient` or an error if the connection fails.
    pub async fn new() -> Result<Self, Box<dyn Error>> {
        let socket_path = CLIENT_UDS_PATH.as_str();
        let uds_uri = format!("http://localhost/{}", socket_path);

        let endpoint = Endpoint::from_shared(uds_uri)
            .map_err(|e| anyhow::anyhow!("Invalid UDS URI: {}", e))?;

        let channel = endpoint
            .connect_with_connector(service_fn(|uri: Uri| async move {
                let path = uri.path();
                let stream = UnixStream::connect(path).await?;
                Ok::<_, std::io::Error>(TokioIo::new(stream))
            }))
            .await?;
        log::info!("Connected client to Isolate EZ Bridge at {}", socket_path);

        Ok(Self { client: IsolateEzBridgeClient::new(channel) })
    }

    /// Performs a unary RPC call to the Enforcer.
    ///
    /// # Arguments
    ///
    /// * `req` - The request message, which can be converted into a `tonic::Request`.
    ///
    /// # Returns
    ///
    /// Returns a `Result` containing the `InvokeEzResponse` or a `tonic::Status` error.
    pub async fn invoke_ez(
        &self,
        req: impl IntoRequest<InvokeEzRequest>,
    ) -> Result<InvokeEzResponse, Status> {
        let mut client = self.client.clone();
        let response = client.invoke_ez(req).await?.into_inner();
        Ok(response)
    }

    /// Performs a streaming RPC call to the Enforcer.
    ///
    /// # Arguments
    ///
    /// * `request` - The stream of request messages.
    ///
    /// # Returns
    ///
    /// Returns a `Result` containing the response stream or a `tonic::Status` error.
    pub async fn stream_invoke_ez(
        &self,
        request: impl IntoStreamingRequest<Message = InvokeEzRequest>,
    ) -> Result<Streaming<InvokeEzResponse>, Status> {
        let mut client = self.client.clone();
        let response = client.stream_invoke_ez(request).await?.into_inner();
        Ok(response)
    }

    /// Notifies the Enforcer of a new Isolate state.
    ///
    /// This typically happens when the Isolate is ready to receive traffic or is shutting down.
    ///
    /// # Arguments
    ///
    /// * `new_isolate_state` - The new state to report.
    ///
    /// # Returns
    ///
    /// Returns a `Result` indicating success or failure.
    pub async fn new_isolate_state(&self, new_isolate_state: IsolateState) -> Result<(), Status> {
        let mut client = self.client.clone();
        let request = NotifyIsolateStateRequest { new_isolate_state: new_isolate_state as i32 };

        // This is a client-streaming RPC in the proto, so we create a stream.
        let stream = tokio_stream::iter(vec![request]);
        let response = client.notify_isolate_state(stream).await?;

        log::info!("response: {:?}", response.into_inner());
        log::info!("Successfully notified of new isolate state: {:?}", new_isolate_state);
        Ok(())
    }

    // TODO: Add shared memory management.
}
