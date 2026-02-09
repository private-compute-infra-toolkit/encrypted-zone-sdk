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
//

use anyhow::Context;
use enforcer_proto::enforcer::v1::ez_isolate_bridge_server::EzIsolateBridge;
use enforcer_proto::enforcer::v1::isolate_ez_bridge_server::IsolateEzBridgeServer;
use enforcer_proto::enforcer::v1::{
    ControlPlaneMetadata, EzPayloadData, InvokeIsolateRequest, InvokeIsolateResponse, IsolateState,
};
use nix::sys::stat::Mode;
use nix::unistd::mkfifo;
use once_cell::sync::Lazy;
use rust_core::{IsolateRpcServer, RpcDispatcher};
use std::env;
use std::error::Error;
use std::path::Path;
use std::sync::Arc;
use test_utils::{
    ErrorIsolateRpcService, MockEzIsolateBridge, MockIsolateEzBridgeServer, MockIsolateRpcService,
    TestEzIsolateBridgeClient,
};
use tokio::fs::{create_dir_all, remove_file, try_exists, OpenOptions};
use tokio::net::UnixListener;
use tokio::sync::mpsc::{channel, Sender};
use tokio::task::JoinHandle;
use tokio::time::{sleep, Duration};
use tokio_stream::wrappers::UnixListenerStream;
use tokio_stream::{self as stream, StreamExt};
use tonic::transport::Server;
use tonic::Code;

static SERVER_UDS_PATH: Lazy<String> =
    Lazy::new(|| env::var("SERVER_UDS_PATH").expect("Required env var"));
static READY_FIFO_PATH: Lazy<String> =
    Lazy::new(|| env::var("READY_FIFO_PATH").expect("Required env var"));
static CLIENT_UDS_PATH: Lazy<String> =
    Lazy::new(|| env::var("CLIENT_UDS_PATH").expect("Required env var"));

#[derive(Debug)]
struct TestHarness<T> {
    client: TestEzIsolateBridgeClient,
    bridge: Arc<T>,
    server_handle: JoinHandle<Result<(), anyhow::Error>>,
    server_shutdown_tx: Sender<()>,
    mock_enforcer_server: MockIsolateEzBridgeServer,
    mock_enforcer_handle: JoinHandle<Result<(), anyhow::Error>>,
    mock_enforcer_shutdown_tx: Sender<()>,
}

impl<T: EzIsolateBridge + Send + Sync + 'static> TestHarness<T> {
    async fn start(bridge: Arc<T>) -> Result<Self, Box<dyn Error>> {
        // Ensure parent directory for UDS exists.
        let server_uds_path = Path::new(SERVER_UDS_PATH.as_str());
        if let Some(parent) = server_uds_path.parent() {
            create_dir_all(parent).await.map_err(|e| {
                anyhow::anyhow!("Failed to create parent dir {}: {}", parent.display(), e)
            })?;
        }
        // Clean up any old socket/FIFO files that might exist.
        for path in [SERVER_UDS_PATH.as_str(), CLIENT_UDS_PATH.as_str(), READY_FIFO_PATH.as_str()] {
            if try_exists(path).await? {
                remove_file(path).await.context(format!("Failed to remove file: {}", path))?;
            }
        }

        // Create the ready signal FIFO path
        mkfifo(READY_FIFO_PATH.as_str(), Mode::S_IRWXU).context("Failed to create FIFO path")?;

        let uds = UnixListener::bind(CLIENT_UDS_PATH.as_str())
            .expect("Should be able to bind to client UDS path");
        let uds_stream = UnixListenerStream::new(uds);

        let bridge_clone = bridge.clone();
        let (server_shutdown_tx, server_shutdown_rx) = channel(1);
        let (mock_enforcer_shutdown_tx, mut mock_enforcer_shutdown_rx) = channel(1);

        // Spawn the server in a separate thread.
        let server_handle = tokio::spawn(async move {
            let server = IsolateRpcServer::new(bridge_clone);
            server.start(Some(server_shutdown_rx)).await;
            Ok(())
        });

        // Spawn the mock enforcer in a separate thread.
        let mock_enforcer_server = MockIsolateEzBridgeServer::default();
        let mock_enforcer_clone = mock_enforcer_server.clone();
        let mock_enforcer_handle = tokio::spawn(async move {
            let mock_enforcer_server = IsolateEzBridgeServer::new(mock_enforcer_clone);
            Server::builder()
                .add_service(mock_enforcer_server)
                .serve_with_incoming_shutdown(uds_stream, async move {
                    mock_enforcer_shutdown_rx.recv().await;
                })
                .await
                .context("Failed to start mock enforcer")
        });

        // Signal the server that it can start accepting connections.
        OpenOptions::new()
            .write(true)
            .open(READY_FIFO_PATH.as_str())
            .await
            .context("Failed to signal ready pipe")?;

        // There is no synchronization mechanism currently to ensure that enforcer client connects
        // after the EZ-Isolate server has started.
        // So sleep for 100ms so that the Isolate starts up the EZ-Isolate Bridge server.
        sleep(Duration::from_millis(100)).await;

        let client = TestEzIsolateBridgeClient::new(SERVER_UDS_PATH.as_str())
            .await
            .context("Failed to start EZ Isolate Bridge Client")?;

        Ok(Self {
            client,
            bridge,
            server_shutdown_tx,
            server_handle,
            mock_enforcer_server,
            mock_enforcer_handle,
            mock_enforcer_shutdown_tx,
        })
    }

    async fn stop(self) -> Result<(), Box<dyn Error>> {
        self.server_shutdown_tx.send(()).await.context("Failed to send shutdown signal")?;
        self.server_handle
            .await
            .context("Failed to wait for server to shut down")?
            .context("Fake enforcer server failed")?;
        self.mock_enforcer_shutdown_tx.send(()).await.context("Failed to send shutdown signal")?;
        self.mock_enforcer_handle
            .await
            .context("Failed to wait for mock enforcer to shut down")?
            .context("Mock enforcer failed")?;
        Ok(())
    }
}

/// Tests the instantiation of the IsolateRpcServer and basic unary RPC invocation.
///
/// This test:
/// 1. Starts a TestHarness which spins up the IsolateRpcServer in a separate task.
/// 2. Verifies that the server is initially in a clean state.
/// 3. Sends a unary `invoke_isolate` request using the client.
/// 4. Verifies the response and that the bridge implementation received the call.
/// 5. Gracefully stops the server.
#[tokio::test]
async fn test_isolate_server_instantiation() {
    let bridge = Arc::new(MockEzIsolateBridge::default());
    let harness = TestHarness::start(bridge).await.expect("Test harness should start");

    // Verify that the bridge has received no calls initially.
    assert_eq!(harness.bridge.call_count(), 0);

    // Send a unary request to the server.
    let response = harness
        .client
        .invoke_isolate(InvokeIsolateRequest::default())
        .await
        .expect("Failed to invoke isolate");

    // Verify the response matches the default response from the mock.
    assert_eq!(response, InvokeIsolateResponse::default());
    // Verify that the bridge invocation count incremented.
    assert_eq!(harness.bridge.call_count(), 1);
    // Verify Isolate State is marked READY
    assert_eq!(
        harness.mock_enforcer_server.last_known_state().await,
        Some(IsolateState::Ready.into())
    );

    harness.stop().await.expect("Test harness should stop");
}

/// Tests successful unary RPC dispatch via RpcDispatcher.
///
/// This test:
/// 1. Starts a TestHarness with RpcDispatcher and a MockIsolateRpcService.
/// 2. Verifies that the service is initially in a clean state.
/// 3. Sends a valid unary `invoke_isolate` request with metadata.
/// 4. Verifies the response contains the expected datagram and metadata.
/// 5. Verifies the service received exactly one unary call.
/// 6. Gracefully stops the server.
#[tokio::test]
async fn test_unary_rpc_dispatch() {
    let mock_service = Arc::new(MockIsolateRpcService::default());
    let dispatcher = Arc::new(RpcDispatcher::new(mock_service.clone()));
    let harness = TestHarness::start(dispatcher).await.expect("Test harness should start");

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    let unary_request = InvokeIsolateRequest {
        control_plane_metadata: Some(ControlPlaneMetadata {
            destination_service_name: "mock_service".to_string(),
            destination_operator_domain: "mock_domain".to_string(),
            destination_ez_instance_id: "mock_instance_id".to_string(),
            destination_method_name: "mock_method".to_string(),
            ipc_message_id: 1234,
            ..Default::default()
        }),
        isolate_input: Some(EzPayloadData { datagrams: vec![b"hello_world".to_vec()] }),
        ..Default::default()
    };

    let response =
        harness.client.invoke_isolate(unary_request).await.expect("Failed to invoke isolate");

    assert_eq!(mock_service.unary_call_count(), 1);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    assert_eq!(response.isolate_output.unwrap().datagrams[0], b"hello_world");
    assert_eq!(response.control_plane_metadata.clone().unwrap().ipc_message_id, 1234);
    assert!(response.control_plane_metadata.clone().unwrap().responder_is_local);

    harness.stop().await.expect("Test harness should stop");
}

/// Tests error handling when control plane metadata is missing in the unary request.
///
/// This test:
/// 1. Starts a TestHarness with RpcDispatcher.
/// 2. Sends a unary request with `control_plane_metadata` set to `None`.
/// 3. Verifies that the server returns an `INVALID_ARGUMENT` (code 3) status.
/// 4. Verifies that the service was not invoked.
/// 5. Gracefully stops the server.
#[tokio::test]
async fn test_unary_rpc_dispatch_with_no_control_plane_metadata() {
    let mock_service = Arc::new(MockIsolateRpcService::default());
    let dispatcher = Arc::new(RpcDispatcher::new(mock_service.clone()));
    let harness = TestHarness::start(dispatcher).await.expect("Test harness should start");

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    let unary_request_with_no_control_plane_metadata =
        InvokeIsolateRequest { control_plane_metadata: None, ..Default::default() };

    let response_with_no_control_plane_metadata = harness
        .client
        .invoke_isolate(unary_request_with_no_control_plane_metadata)
        .await
        .expect("Failed to invoke isolate");

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    assert_eq!(
        response_with_no_control_plane_metadata.status.expect("Should have error status").code,
        Code::InvalidArgument as i32,
    );

    harness.stop().await.expect("Test harness should stop");
}
/// Tests error handling when the destination service name is not found.
///
/// This test:
/// 1. Starts a TestHarness with RpcDispatcher.
/// 2. Sends a unary request with a `destination_service_name` that does not exist in the dispatcher.
/// 3. Verifies that the server returns a `NOT_FOUND` (code 5) status.
/// 4. Verifies that the service was not invoked.
/// 5. Gracefully stops the server.
#[tokio::test]
async fn test_unary_rpc_dispatch_with_wrong_destination_service_name() {
    let mock_service = Arc::new(MockIsolateRpcService::default());
    let dispatcher = Arc::new(RpcDispatcher::new(mock_service.clone()));
    let harness = TestHarness::start(dispatcher).await.expect("Test harness should start");

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    let unary_request_with_wrong_destination_service_name = InvokeIsolateRequest {
        control_plane_metadata: Some(ControlPlaneMetadata {
            destination_service_name: "wrong_service_name".to_string(),
            ..Default::default()
        }),
        ..Default::default()
    };

    let response_with_wrong_destination_service_name = harness
        .client
        .invoke_isolate(unary_request_with_wrong_destination_service_name)
        .await
        .expect("Failed to invoke isolate");

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    assert_eq!(
        response_with_wrong_destination_service_name.status.expect("Should have error status").code,
        Code::NotFound as i32,
    );

    harness.stop().await.expect("Test harness should stop");
}

/// Tests error handling when the request datagram is missing.
///
/// This test:
/// 1. Starts a TestHarness with RpcDispatcher.
/// 2. Sends a unary request with `isolate_input` (datagram) set to `None`.
/// 3. Verifies that the server returns an `INVALID_ARGUMENT` (code 3) status.
/// 4. Verifies that the service was not invoked.
/// 5. Gracefully stops the server.
#[tokio::test]
async fn test_unary_rpc_dispatch_with_no_datagram() {
    let mock_service = Arc::new(MockIsolateRpcService::default());
    let dispatcher = Arc::new(RpcDispatcher::new(mock_service.clone()));
    let harness = TestHarness::start(dispatcher).await.expect("Test harness should start");

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    let unary_request_with_no_datagram = InvokeIsolateRequest {
        control_plane_metadata: Some(ControlPlaneMetadata {
            destination_operator_domain: "mock_domain".to_string(),
            destination_service_name: "mock_service".to_string(),
            destination_method_name: "mock_method".to_string(),
            destination_ez_instance_id: "mock_instance_id".to_string(),
            ipc_message_id: 1234,
            ..Default::default()
        }),
        isolate_input: None,
        ..Default::default()
    };

    let response_with_no_datagram = harness
        .client
        .invoke_isolate(unary_request_with_no_datagram)
        .await
        .expect("Failed to invoke isolate");

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    assert_eq!(
        response_with_no_datagram.status.expect("Should have error status").code,
        Code::InvalidArgument as i32,
    );

    harness.stop().await.expect("Test harness should stop");
}

/// Tests error handling when the destination method name is incorrect.
///
/// This test:
/// 1. Starts a TestHarness with RpcDispatcher.
/// 2. Sends a unary request with a `destination_method_name` that is rejected by the mock service.
/// 3. Verifies that the server returns an `INVALID_ARGUMENT` (code 3) status from the service handler.
/// 4. Verifies that the service was not invoked (or invoked but returned error, in this case counting as no successful call depending on implementation, but actually the mock logic checks method name before incrementing count, so count is 0).
/// 5. Gracefully stops the server.
#[tokio::test]
async fn test_unary_rpc_dispatch_with_wrong_method_name() {
    let mock_service = Arc::new(MockIsolateRpcService::default());
    let dispatcher = Arc::new(RpcDispatcher::new(mock_service.clone()));
    let harness = TestHarness::start(dispatcher).await.expect("Test harness should start");

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    let unary_request_with_wrong_method_name = InvokeIsolateRequest {
        control_plane_metadata: Some(ControlPlaneMetadata {
            destination_operator_domain: "mock_domain".to_string(),
            destination_service_name: "mock_service".to_string(),
            destination_method_name: "wrong_method_name".to_string(),
            destination_ez_instance_id: "mock_instance_id".to_string(),
            ipc_message_id: 1234,
            ..Default::default()
        }),
        isolate_input: None,
        ..Default::default()
    };

    let response_with_wrong_method_name = harness
        .client
        .invoke_isolate(unary_request_with_wrong_method_name)
        .await
        .expect("Failed to invoke isolate");

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    assert_eq!(
        response_with_wrong_method_name.status.expect("Should have error status").code,
        Code::InvalidArgument as i32,
    );

    harness.stop().await.expect("Test harness should stop");
}

/// Tests successful streaming RPC dispatch via RpcDispatcher.
///
/// This test:
/// 1. Starts a TestHarness with RpcDispatcher and a MockIsolateRpcService.
/// 2. Verifies that the service is initially in a clean state.
/// 3. Creates a stream of 5 `invoke_isolate` requests with sequential IPC message IDs.
/// 4. Sends the stream of requests using the client.
/// 5. Verifies that 5 responses are received and each has the expected metadata.
/// 6. Verifies that the service received exactly one stream call with 5 messages.
/// 7. Gracefully stops the server.
#[tokio::test]
async fn test_stream_rpc_dispatch() {
    let mock_service = Arc::new(MockIsolateRpcService::default());
    let dispatcher = Arc::new(RpcDispatcher::new(mock_service.clone()));
    let harness = TestHarness::start(dispatcher).await.expect("Test harness should start");

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    let stream_request = InvokeIsolateRequest {
        control_plane_metadata: Some(ControlPlaneMetadata {
            destination_operator_domain: "mock_domain".to_string(),
            destination_service_name: "mock_service".to_string(),
            destination_method_name: "mock_method".to_string(),
            destination_ez_instance_id: "mock_instance_id".to_string(),
            ipc_message_id: 1230,
            ..Default::default()
        }),
        isolate_input: Some(EzPayloadData { datagrams: vec![b"hello_world".to_vec()] }),
        ..Default::default()
    };

    let requests: Vec<_> = (0..5)
        .map(|i| {
            let mut req = stream_request.clone();
            if let Some(metadata) = &mut req.control_plane_metadata {
                metadata.ipc_message_id = 1230 + i;
            }
            req
        })
        .collect();
    let input_stream = stream::iter(requests);

    let mut response_stream = harness
        .client
        .stream_invoke_isolate(input_stream)
        .await
        .expect("Failed to stream invoke isolate");

    let mut responses = Vec::new();
    while let Some(response) = response_stream.next().await {
        responses.push(response.expect("Failed to get response"));
    }

    assert_eq!(responses.len(), 5);
    for response in responses.iter() {
        assert_eq!(response.control_plane_metadata.as_ref().unwrap().ipc_message_id, 1230);
    }

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 1);
    assert_eq!(mock_service.stream_message_count(), 5);

    harness.stop().await.expect("Test harness should stop");
}

/// Tests streaming RPC dispatch with an empty input stream.
///
/// This test:
/// 1. Starts a TestHarness with RpcDispatcher.
/// 2. Sends an empty stream of requests using the client.
/// 3. Verifies that the server returns a single response with `INVALID_ARGUMENT` status (code 3).
/// 4. Verifies that the service was not invoked.
/// 5. Gracefully stops the server.
#[tokio::test]
async fn test_stream_rpc_dispatch_with_empty_stream() {
    let mock_service = Arc::new(MockIsolateRpcService::default());
    let dispatcher = Arc::new(RpcDispatcher::new(mock_service.clone()));
    let harness = TestHarness::start(dispatcher).await.expect("Test harness should start");

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    let mut response_stream = harness
        .client
        .stream_invoke_isolate(stream::iter(vec![]))
        .await
        .expect("Failed to stream invoke isolate");

    let mut responses = Vec::new();
    while let Some(response) = response_stream.next().await {
        responses.push(response.expect("Failed to get response"));
    }

    assert_eq!(responses.len(), 1);
    assert_eq!(responses[0].status.as_ref().expect("Should have error status").code, 3); // INVALID_ARGUMENT

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    harness.stop().await.expect("Test harness should stop");
}

/// Tests error handling when the destination service name is incorrect in a streaming request.
///
/// This test:
/// 1. Starts a TestHarness with RpcDispatcher.
/// 2. Creates a stream of requests with an incorrect destination service name.
/// 3. Sends the stream using the client.
/// 4. Verifies that the server returns a response with `NOT_FOUND` status (code 5).
/// 5. Verifies that the service was not invoked.
/// 6. Gracefully stops the server.
#[tokio::test]
async fn test_stream_rpc_dispatch_with_wrong_service_name() {
    let mock_service = Arc::new(MockIsolateRpcService::default());
    let dispatcher = Arc::new(RpcDispatcher::new(mock_service.clone()));
    let harness = TestHarness::start(dispatcher).await.expect("Test harness should start");

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    let stream_request = InvokeIsolateRequest {
        control_plane_metadata: Some(ControlPlaneMetadata {
            destination_operator_domain: "mock_domain".to_string(),
            destination_service_name: "wrong_service_name".to_string(),
            destination_method_name: "mock_method".to_string(),
            destination_ez_instance_id: "mock_instance_id".to_string(),
            ipc_message_id: 1230,
            ..Default::default()
        }),
        isolate_input: Some(EzPayloadData { datagrams: vec![b"hello_world".to_vec()] }),
        ..Default::default()
    };

    let requests: Vec<_> = (0..5)
        .map(|i| {
            let mut req = stream_request.clone();
            if let Some(metadata) = &mut req.control_plane_metadata {
                metadata.ipc_message_id = 1230 + i;
            }
            req
        })
        .collect();
    let input_stream = stream::iter(requests);

    let mut response_stream = harness
        .client
        .stream_invoke_isolate(input_stream)
        .await
        .expect("Failed to stream invoke isolate");

    let mut responses = Vec::new();
    while let Some(response) = response_stream.next().await {
        responses.push(response.expect("Failed to get response"));
    }

    assert_eq!(responses.len(), 1);
    assert_eq!(responses[0].status.as_ref().expect("Should have error status").code, 5); // NOT_FOUND

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    harness.stop().await.expect("Test harness should stop");
}

/// Tests error handling when the destination method name is incorrect in a streaming request.
///
/// This test:
/// 1. Starts a TestHarness with RpcDispatcher.
/// 2. Creates a stream of requests with an incorrect destination method name.
/// 3. Sends the stream using the client.
/// 4. Verifies that the stream yields an error status (INVALID_ARGUMENT).
/// 5. Verifies that the service was not invoked.
/// 6. Gracefully stops the server.
#[tokio::test]
async fn test_stream_rpc_dispatch_with_wrong_method_name() {
    let mock_service = Arc::new(MockIsolateRpcService::default());
    let dispatcher = Arc::new(RpcDispatcher::new(mock_service.clone()));
    let harness = TestHarness::start(dispatcher).await.expect("Test harness should start");

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    let stream_request = InvokeIsolateRequest {
        control_plane_metadata: Some(ControlPlaneMetadata {
            destination_operator_domain: "mock_domain".to_string(),
            destination_service_name: "mock_service".to_string(),
            destination_method_name: "wrong_method_name".to_string(),
            destination_ez_instance_id: "mock_instance_id".to_string(),
            ipc_message_id: 1230,
            ..Default::default()
        }),
        isolate_input: Some(EzPayloadData { datagrams: vec![b"hello_world".to_vec()] }),
        ..Default::default()
    };

    let requests: Vec<_> = (0..5)
        .map(|i| {
            let mut req = stream_request.clone();
            if let Some(metadata) = &mut req.control_plane_metadata {
                metadata.ipc_message_id = 1230 + i;
            }
            req
        })
        .collect();
    let input_stream = stream::iter(requests);

    let mut response_stream = harness
        .client
        .stream_invoke_isolate(input_stream)
        .await
        .expect("Failed to stream invoke isolate");

    let mut responses = Vec::new();
    while let Some(response) = response_stream.next().await {
        responses.push(response.expect_err("Should have error status"));
    }

    assert_eq!(responses.len(), 1);
    assert_eq!(responses[0].code(), tonic::Code::InvalidArgument);

    assert_eq!(mock_service.unary_call_count(), 0);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    harness.stop().await.expect("Test harness should stop");
}

/// Tests dispatching requests to multiple registered services.
///
/// This test:
/// 1. Registers both a `MockIsolateRpcService` and an `ErrorIsolateRpcService` with the dispatcher.
/// 2. Starts a TestHarness.
/// 3. Sends a request to the `mock_service` and verifies it succeeds.
/// 4. Sends a request to the `error_service` and verifies it returns an error (UNIMPLEMENTED).
#[tokio::test]
async fn test_stream_rpc_dispatch_with_multiple_services() {
    let mock_service = Arc::new(MockIsolateRpcService::default());
    let error_service = Arc::new(ErrorIsolateRpcService);
    let dispatcher = Arc::new(RpcDispatcher::new(mock_service.clone()));
    dispatcher.add_service(error_service).await;
    let harness = TestHarness::start(dispatcher).await.expect("Test harness should start");

    let unary_request = InvokeIsolateRequest {
        control_plane_metadata: Some(ControlPlaneMetadata {
            destination_service_name: "mock_service".to_string(),
            destination_operator_domain: "mock_domain".to_string(),
            destination_ez_instance_id: "mock_instance_id".to_string(),
            destination_method_name: "mock_method".to_string(),
            ipc_message_id: 1234,
            ..Default::default()
        }),
        isolate_input: Some(EzPayloadData { datagrams: vec![b"hello_world".to_vec()] }),
        ..Default::default()
    };

    let response =
        harness.client.invoke_isolate(unary_request).await.expect("Failed to invoke isolate");

    assert_eq!(mock_service.unary_call_count(), 1);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    assert_eq!(response.isolate_output.unwrap().datagrams[0], b"hello_world");
    assert_eq!(response.control_plane_metadata.clone().unwrap().ipc_message_id, 1234);
    assert!(response.control_plane_metadata.clone().unwrap().responder_is_local);

    let unary_request = InvokeIsolateRequest {
        control_plane_metadata: Some(ControlPlaneMetadata {
            destination_service_name: "error_service".to_string(),
            destination_operator_domain: "error_domain".to_string(),
            destination_ez_instance_id: "error_instance_id".to_string(),
            destination_method_name: "error_method".to_string(),
            ipc_message_id: 1234,
            ..Default::default()
        }),
        isolate_input: Some(EzPayloadData { datagrams: vec![b"hello_world".to_vec()] }),
        ..Default::default()
    };

    let response =
        harness.client.invoke_isolate(unary_request).await.expect("Failed to invoke isolate");

    assert!(response.status.is_some());
    assert_eq!(response.status.as_ref().unwrap().code, Code::Unimplemented as i32);

    assert_eq!(mock_service.unary_call_count(), 1);
    assert_eq!(mock_service.stream_call_count(), 0);
    assert_eq!(mock_service.stream_message_count(), 0);

    harness.stop().await.expect("Test harness should stop");
}

/// Tests the state transitions for UpdateIsolateState.
#[tokio::test]
async fn test_update_isolate_state() {
    let mock_service = Arc::new(MockIsolateRpcService::default());
    let dispatcher = Arc::new(RpcDispatcher::new(mock_service.clone()));
    let harness = TestHarness::start(dispatcher).await.expect("Test harness should start");

    // Test state transitions
    let test_cases = vec![
        (IsolateState::Starting, IsolateState::Ready),
        (IsolateState::Ready, IsolateState::Ready),
        (IsolateState::Retiring, IsolateState::Retiring),
        (IsolateState::Idle, IsolateState::Idle),
    ];

    for (input_state, expected_state) in test_cases {
        let response = harness
            .client
            .update_isolate_state(enforcer_proto::enforcer::v1::UpdateIsolateStateRequest {
                move_to_state: input_state as i32,
            })
            .await
            .expect("Failed to update isolate state");
        assert_eq!(response.current_state, expected_state as i32);
    }

    harness.stop().await.expect("Test harness should stop");
}

/// Tests that requests succeed in Ready state but fail after transitioning to Retiring.
#[tokio::test]
async fn test_request_lifecycle_with_state_change() {
    let mock_service = Arc::new(MockIsolateRpcService::default());
    let dispatcher = Arc::new(RpcDispatcher::new(mock_service.clone()));
    let harness = TestHarness::start(dispatcher).await.expect("Test harness should start");

    let request = InvokeIsolateRequest {
        control_plane_metadata: Some(ControlPlaneMetadata {
            destination_service_name: "mock_service".to_string(),
            destination_operator_domain: "mock_domain".to_string(),
            destination_ez_instance_id: "mock_instance_id".to_string(),
            destination_method_name: "mock_method".to_string(),
            ipc_message_id: 1111,
            ..Default::default()
        }),
        isolate_input: Some(EzPayloadData { datagrams: vec![b"test".to_vec()] }),
        ..Default::default()
    };

    // 1. Should succeed in Ready state
    let response =
        harness.client.invoke_isolate(request.clone()).await.expect("Failed to invoke in Ready");
    if let Some(status) = response.status {
        assert_eq!(status.code, Code::Ok as i32, "Expected Ok status in Ready state");
    }

    // 2. Move to Retiring
    harness
        .client
        .update_isolate_state(enforcer_proto::enforcer::v1::UpdateIsolateStateRequest {
            move_to_state: IsolateState::Retiring as i32,
        })
        .await
        .expect("Failed to update state");

    // 3. Should fail in Retiring state
    let response =
        harness.client.invoke_isolate(request).await.expect("Failed to invoke in Retiring");

    // Expect failure
    let status = response.status.expect("Expected status in Retiring state");
    assert_ne!(status.code, Code::Ok as i32, "Expected non-Ok code in Retiring state");

    harness.stop().await.expect("Test harness should stop");
}
