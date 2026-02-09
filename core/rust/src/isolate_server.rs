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

use crate::IsolateEzBridgeSdkClient;
use enforcer_proto::enforcer::v1::ez_isolate_bridge_server::{
    EzIsolateBridge, EzIsolateBridgeServer,
};
use enforcer_proto::enforcer::v1::IsolateState;
use once_cell::sync::Lazy;
use std::env;
use std::path::Path;
use std::sync::Arc;
use tokio::fs::remove_file;
use tokio::fs::OpenOptions;
use tokio::net::UnixListener;
use tokio::sync::mpsc::Receiver;
use tokio::sync::OnceCell;
use tokio_stream::wrappers::UnixListenerStream;
use tonic::transport::Server;

static SERVER_UDS_PATH: Lazy<String> = Lazy::new(|| {
    env::var("SERVER_UDS_PATH")
        .unwrap_or_else(|_| "/enforcer-isolate-shared/ez-isolate-bridge-uds".to_string())
});

static READY_FIFO_PATH: Lazy<String> = Lazy::new(|| {
    env::var("READY_FIFO_PATH")
        .unwrap_or_else(|_| "/enforcer-isolate-shared/isolate-ez-bridge-uds.ready".to_string())
});

/// The main server object that an Isolate developer will interact with.
///
/// This object takes a generic implementation of the bridge and starts the EZ Isolate Server.
/// It will listen on a predefined Unix Domain Socket and block the current thread.
#[derive(Debug)]
pub struct IsolateRpcServer<T> {
    bridge: Arc<T>,
    client: OnceCell<Arc<IsolateEzBridgeSdkClient>>,
}

impl<T: EzIsolateBridge + Send + Sync + 'static> IsolateRpcServer<T> {
    /// Creates a new `IsolateRpcServer`, registering the provided bridge implementation.
    ///
    /// Note: The client is lazily initialized. You must call `init_client().await`
    /// or `start().await` to initialize it.
    ///
    /// # Arguments
    ///
    /// * `bridge` - The bridge implementation to use for handling RPCs.
    pub fn new(bridge: Arc<T>) -> Self {
        Self { bridge, client: OnceCell::new() }
    }

    /// Initializes the Isolate EZ Bridge client if it hasn't been initialized yet.
    ///
    /// This method waits for the ready signal from the Enforcer before creating the client.
    ///
    /// # Returns
    ///
    /// Returns a shared `Arc<IsolateEzBridgeSdkClient>`.
    async fn init_client(&self) -> Arc<IsolateEzBridgeSdkClient> {
        OpenOptions::new()
            .read(true)
            .open(READY_FIFO_PATH.as_str())
            .await
            .expect("Failed to open ready pipe");

        log::info!("Received the ready signal from the Enforcer.");
        Arc::new(IsolateEzBridgeSdkClient::new().await.expect("Failed to create client"))
    }

    /// Retrieves the shared Isolate EZ Bridge client instance.
    ///
    /// All Isolate Stubs should use this client instance provided by the `IsolateRpcServer`.
    /// This ensures a single, consistent client for interacting with the Isolate EZ Bridge.
    ///
    /// # Returns
    ///
    /// Returns a shared `Arc<IsolateEzBridgeSdkClient>`.
    pub async fn get_isolate_ez_bridge_client(&self) -> Arc<IsolateEzBridgeSdkClient> {
        self.client.get_or_init(|| async { self.init_client().await }).await.clone()
    }

    /// Starts the gRPC bridge server and blocks until it is terminated.
    ///
    /// This function performs the following steps:
    /// 1. Binds to the predefined Unix Domain Socket (UDS).
    /// 2. Notifies the enforcer that the Isolate is READY.
    /// 3. Starts serving gRPC traffic.
    ///
    /// # Arguments
    ///
    /// * `shutdown_rx` - Optional receiver for a shutdown signal. If provided, the server will
    ///   stop when a message is received.
    pub async fn start(&self, shutdown_rx: Option<Receiver<()>>) {
        let socket_path = SERVER_UDS_PATH.as_str();
        // Clean up any old socket file that might exist.
        if Path::new(socket_path).exists() {
            remove_file(socket_path)
                .await
                .unwrap_or_else(|e| panic!("Failed to remove file {}: {}", socket_path, e));
        }
        log::info!("Starting EzIsolateBridge Server. Socket path: {}", socket_path);

        let uds = UnixListener::bind(socket_path).expect("Failed to bind to socket path");
        let uds_stream = UnixListenerStream::new(uds);

        let bridge_server = EzIsolateBridgeServer::from_arc(self.bridge.clone());

        // Notify the enforcer that we are ready to serve traffic.
        // We must get a lock on the singleton client instance.
        log::info!("Notifying enforcer that Isolate is READY.");
        self.get_isolate_ez_bridge_client()
            .await
            .new_isolate_state(IsolateState::Ready)
            .await
            .expect("Should be able to set Isolate State to Ready");
        log::info!("Server is ready and listening on {}", socket_path);

        Server::builder()
            .add_service(bridge_server)
            .serve_with_incoming_shutdown(uds_stream, async move {
                if let Some(mut rx) = shutdown_rx {
                    rx.recv().await;
                } else {
                    std::future::pending::<()>().await;
                }
            })
            .await
            .expect("Failed to start EZ Isolate Bridge Server");
    }
}
