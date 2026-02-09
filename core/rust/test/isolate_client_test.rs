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
use enforcer_proto::data_scope_proto::enforcer::v1::DataScopeType;
use enforcer_proto::enforcer::v1::isolate_ez_bridge_server::IsolateEzBridgeServer;
use enforcer_proto::enforcer::v1::{
    ControlPlaneMetadata, EzPayloadData, EzPayloadIsolateScope, InvokeEzRequest, IsolateDataScope,
    IsolateState,
};
use once_cell::sync::Lazy;
use rust_core::{IsolateEzBridgeSdkClient, RpcHandler};
use std::env;
use std::error::Error;
use std::path::Path;
use std::sync::Arc;
use test_message_proto::test_message::TestMessage;
use test_utils::MockIsolateEzBridgeServer;
use tokio::fs::{create_dir_all, remove_file, try_exists};
use tokio::net::UnixListener;
use tokio::sync::mpsc::channel;
use tokio::sync::mpsc::Sender;
use tokio::task::JoinHandle;
use tokio_stream::wrappers::UnixListenerStream;
use tokio_stream::{self as stream, StreamExt};
use tonic::transport::Server;
use tonic::Request;

static CLIENT_UDS_PATH: Lazy<String> =
    Lazy::new(|| env::var("CLIENT_UDS_PATH").expect("Required env var"));

struct TestHarness {
    client: Arc<IsolateEzBridgeSdkClient>,
    mock_enforcer_server: MockIsolateEzBridgeServer,
    mock_enforcer_handle: JoinHandle<Result<(), anyhow::Error>>,
    mock_enforcer_shutdown_tx: Sender<()>,
}

impl TestHarness {
    async fn start() -> Result<Self, Box<dyn Error>> {
        // Spawn the mock enforcer in a separate thread.
        let socket_path = Path::new(CLIENT_UDS_PATH.as_str());
        // Clean up any old socket file that might exist.
        if let Some(parent) = socket_path.parent() {
            create_dir_all(parent).await.map_err(|e| {
                anyhow::anyhow!("Failed to create parent dir {}: {}", parent.display(), e)
            })?;
        }
        // Clean up any old socket/FIFO files that might exist.
        if try_exists(socket_path).await? {
            remove_file(socket_path).await.context("Failed to remove file")?;
        }

        let uds = UnixListener::bind(socket_path).expect("Failed to bind to socket path");
        let uds_stream = UnixListenerStream::new(uds);

        let (mock_enforcer_shutdown_tx, mut mock_enforcer_shutdown_rx) = channel(1);
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

        Ok(Self {
            client: Arc::new(IsolateEzBridgeSdkClient::new().await?),
            mock_enforcer_server,
            mock_enforcer_handle,
            mock_enforcer_shutdown_tx,
        })
    }

    async fn stop(self) -> Result<(), Box<dyn Error>> {
        self.mock_enforcer_shutdown_tx.send(()).await.context("Failed to send shutdown signal")?;
        self.mock_enforcer_handle
            .await
            .context("Failed to wait for mock enforcer to shut down")?
            .context("Mock enforcer failed")?;
        Ok(())
    }
}

#[tokio::test]
async fn test_isolate_client_unary() {
    let harness = TestHarness::start().await.expect("Test harness should start");

    let response = harness
        .client
        .invoke_ez(InvokeEzRequest {
            control_plane_metadata: Some(ControlPlaneMetadata {
                ipc_message_id: 1234,
                destination_ez_instance_id: "test_instance_id".to_string(),
                destination_method_name: "test_method_name".to_string(),
                destination_operator_domain: "test_operator_domain".to_string(),
                destination_service_name: "test_service_name".to_string(),
                requester_is_local: true,
                ..Default::default()
            }),
            isolate_request_payload: Some(EzPayloadData {
                datagrams: vec![b"test_payload".to_vec()],
            }),
            isolate_request_iscope: Some(EzPayloadIsolateScope {
                datagram_iscopes: vec![IsolateDataScope {
                    scope_type: DataScopeType::UserPrivate as i32,
                    ..Default::default()
                }],
            }),
        })
        .await
        .expect("Failed to invoke isolate");

    let response_control_plane_metadata = response.control_plane_metadata.unwrap();
    assert_eq!(response_control_plane_metadata.ipc_message_id, 1234);
    assert_eq!(response_control_plane_metadata.destination_ez_instance_id, "mock_instance_id");
    assert_eq!(response_control_plane_metadata.destination_method_name, "mock_method_name");
    assert_eq!(response_control_plane_metadata.destination_operator_domain, "mock_operator_domain");
    assert_eq!(response_control_plane_metadata.destination_service_name, "mock_service_name");
    assert!(response_control_plane_metadata.responder_is_local);
    assert_eq!(response.ez_response_payload.unwrap().datagrams, vec![b"test_payload".to_vec()]);

    assert_eq!(harness.mock_enforcer_server.unary_call_count(), 1);
    assert_eq!(harness.mock_enforcer_server.stream_call_count(), 0);
    assert_eq!(harness.mock_enforcer_server.stream_message_count(), 0);

    harness.stop().await.expect("Test harness should stop");
}

#[tokio::test]
async fn test_isolate_client_stream() {
    let harness = TestHarness::start().await.expect("Test harness should start");

    let stream_request = InvokeEzRequest {
        control_plane_metadata: Some(ControlPlaneMetadata {
            ipc_message_id: 1230,
            destination_ez_instance_id: "test_instance_id".to_string(),
            destination_method_name: "test_method_name".to_string(),
            destination_operator_domain: "test_operator_domain".to_string(),
            destination_service_name: "test_service_name".to_string(),
            requester_is_local: true,
            ..Default::default()
        }),
        isolate_request_payload: Some(EzPayloadData { datagrams: vec![b"test_payload".to_vec()] }),
        isolate_request_iscope: Some(EzPayloadIsolateScope {
            datagram_iscopes: vec![IsolateDataScope {
                scope_type: DataScopeType::UserPrivate as i32,
                ..Default::default()
            }],
        }),
    };

    let requests = vec![stream_request.clone(); 5];
    let input_stream = stream::iter(requests);

    let mut response_stream = harness
        .client
        .stream_invoke_ez(input_stream)
        .await
        .expect("Failed to stream invoke isolate");

    let mut responses = Vec::new();
    while let Some(response) = response_stream.next().await {
        responses.push(response.expect("Failed to get response"));
    }

    assert_eq!(responses.len(), 5);
    assert_eq!(harness.mock_enforcer_server.unary_call_count(), 0);
    assert_eq!(harness.mock_enforcer_server.stream_call_count(), 1);
    assert_eq!(harness.mock_enforcer_server.stream_message_count(), 5);

    harness.stop().await.expect("Test harness should stop");
}

#[tokio::test]
async fn test_isolate_client_notify_state() {
    let harness = TestHarness::start().await.expect("Test harness should start");

    harness
        .client
        .new_isolate_state(IsolateState::Ready)
        .await
        .expect("Failed to notify isolate state");
    let last_known_state = harness.mock_enforcer_server.last_known_state().await;
    assert_eq!(last_known_state, Some(IsolateState::Ready as i32));

    harness.stop().await.expect("Test harness should stop");
}

#[tokio::test]
async fn test_rpc_handler_unary() {
    let harness = TestHarness::start().await.expect("Test harness should start");

    let rpc_handler = RpcHandler::new(
        harness.client.clone(),
        "test_operator_domain".to_string(),
        "test_service_name".to_string(),
        DataScopeType::UserPrivate,
    );
    let response = rpc_handler
        .isolate_rpc_call::<TestMessage, TestMessage>(
            "test_method",
            TestMessage { field1: "test1".to_string(), field2: "test2".to_string() },
        )
        .await
        .expect("Failed to invoke rpc call");

    assert_eq!(response, TestMessage { field1: "test1".to_string(), field2: "test2".to_string() });

    assert_eq!(harness.mock_enforcer_server.unary_call_count(), 1);
    assert_eq!(harness.mock_enforcer_server.stream_call_count(), 0);
    assert_eq!(harness.mock_enforcer_server.stream_message_count(), 0);

    harness.stop().await.expect("Test harness should stop");
}

#[tokio::test]
async fn test_rpc_handler_stream() {
    let harness = TestHarness::start().await.expect("Test harness should start");

    let rpc_handler = RpcHandler::new(
        harness.client.clone(),
        "test_operator_domain".to_string(),
        "test_service_name".to_string(),
        DataScopeType::UserPrivate,
    );

    let stream_test_request =
        TestMessage { field1: "test1".to_string(), field2: "test2".to_string() };
    let request_stream = tokio_stream::iter(vec![stream_test_request.clone(); 5]);

    let mut response_stream = rpc_handler
        .stream_isolate_rpc_call::<TestMessage, TestMessage>(
            "test_method",
            Request::new(request_stream),
        )
        .await
        .expect("Failed to invoke rpc stream call")
        .into_inner();

    let mut responses = Vec::new();
    while let Some(response) = response_stream.next().await {
        responses.push(response.expect("Failed to get response"));
    }

    assert_eq!(responses.len(), 5);
    assert_eq!(harness.mock_enforcer_server.unary_call_count(), 0);
    assert_eq!(harness.mock_enforcer_server.stream_call_count(), 1);
    assert_eq!(harness.mock_enforcer_server.stream_message_count(), 5);

    assert_eq!(
        responses[0],
        TestMessage { field1: "test1".to_string(), field2: "test2".to_string() }
    );

    harness.stop().await.expect("Test harness should stop");
}
