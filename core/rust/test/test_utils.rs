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

use derivative::Derivative;
use enforcer_proto::data_scope_proto::enforcer::v1::DataScopeType;
use enforcer_proto::enforcer::v1::ez_isolate_bridge_client::EzIsolateBridgeClient;
use enforcer_proto::enforcer::v1::ez_isolate_bridge_server::EzIsolateBridge;
use enforcer_proto::enforcer::v1::isolate_ez_bridge_server::IsolateEzBridge;
use enforcer_proto::enforcer::v1::{
    ControlPlaneMetadata, CreateMemshareRequest, CreateMemshareResponse, InvokeEzRequest,
    InvokeEzResponse, InvokeIsolateRequest, InvokeIsolateResponse, NotifyIsolateStateRequest,
    NotifyIsolateStateResponse, PollIsolateStateRequest, PollIsolateStateResponse,
};
use hyper_util::rt::TokioIo;
use rust_core::{
    GrpcResponseStream, IsolateRpcService, PeekableInvokeIsolateRequestStream,
    PinBoxInvokeIsolateResponseStream,
};
use std::pin::Pin;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use tokio::net::UnixStream;
use tokio::sync::Mutex;
use tokio_stream::StreamExt;
use tonic::transport::{Channel, Endpoint, Uri};
use tonic::{IntoStreamingRequest, Request, Response, Status, Streaming};
use tower::service_fn;

#[derive(Debug, Derivative)]
#[derivative(Default)]
pub struct MockEzIsolateBridge {
    #[derivative(Default(value = "AtomicUsize::new(0)"))]
    call_count: AtomicUsize,
}

impl MockEzIsolateBridge {
    pub fn call_count(&self) -> usize {
        self.call_count.load(Ordering::SeqCst)
    }
}

#[tonic::async_trait]
impl EzIsolateBridge for MockEzIsolateBridge {
    async fn invoke_isolate(
        &self,
        _request: Request<InvokeIsolateRequest>,
    ) -> Result<Response<InvokeIsolateResponse>, Status> {
        self.call_count.fetch_add(1, Ordering::SeqCst);
        Ok(Response::new(InvokeIsolateResponse::default()))
    }

    type StreamInvokeIsolateStream = PinBoxInvokeIsolateResponseStream;
    async fn stream_invoke_isolate(
        &self,
        _request: Request<tonic::Streaming<InvokeIsolateRequest>>,
    ) -> Result<Response<Self::StreamInvokeIsolateStream>, Status> {
        Err(Status::unimplemented("no streaming"))
    }

    async fn update_isolate_state(
        &self,
        _request: Request<enforcer_proto::enforcer::v1::UpdateIsolateStateRequest>,
    ) -> Result<Response<enforcer_proto::enforcer::v1::UpdateIsolateStateResponse>, Status> {
        self.call_count.fetch_add(1, Ordering::SeqCst);
        Ok(Response::new(enforcer_proto::enforcer::v1::UpdateIsolateStateResponse {
            current_state: enforcer_proto::enforcer::v1::IsolateState::Ready as i32,
        }))
    }
}

#[derive(Debug)]
pub struct TestEzIsolateBridgeClient {
    client: EzIsolateBridgeClient<Channel>,
}

impl TestEzIsolateBridgeClient {
    /// Establishes the connection to the gRPC server over a Unix Domain Socket.
    pub async fn new(server_uds_path: &str) -> Result<Self, anyhow::Error> {
        let uds_uri = format!("http://localhost/{}", server_uds_path);

        let endpoint = Endpoint::from_shared(uds_uri)
            .map_err(|e| anyhow::anyhow!("Invalid UDS URI: {}", e))?;

        // TODO: Add retries if this tests starts to flake
        let channel = endpoint.connect_with_connector_lazy(service_fn(|uri: Uri| async move {
            let path = uri.path();
            let stream = UnixStream::connect(path).await?;
            Ok::<_, std::io::Error>(TokioIo::new(stream))
        }));

        Ok(Self { client: EzIsolateBridgeClient::new(channel) })
    }

    pub async fn invoke_isolate(
        &self,
        request: InvokeIsolateRequest,
    ) -> Result<InvokeIsolateResponse, Status> {
        let mut client = self.client.clone();
        let response = client.invoke_isolate(request).await?.into_inner();
        Ok(response)
    }

    pub async fn stream_invoke_isolate(
        &self,
        request: impl IntoStreamingRequest<Message = InvokeIsolateRequest>,
    ) -> Result<Streaming<InvokeIsolateResponse>, Status> {
        let mut client = self.client.clone();
        let response = client.stream_invoke_isolate(request).await?.into_inner();
        Ok(response)
    }

    pub async fn update_isolate_state(
        &self,
        request: enforcer_proto::enforcer::v1::UpdateIsolateStateRequest,
    ) -> Result<enforcer_proto::enforcer::v1::UpdateIsolateStateResponse, Status> {
        let mut client = self.client.clone();
        let response = client.update_isolate_state(request).await?.into_inner();
        Ok(response)
    }
}

#[derive(Debug, Derivative)]
#[derivative(Default)]
pub struct MockIsolateRpcService {
    #[derivative(Default(value = "Arc::new(AtomicUsize::new(0))"))]
    unary_call_count: Arc<AtomicUsize>,
    #[derivative(Default(value = "Arc::new(AtomicUsize::new(0))"))]
    stream_call_count: Arc<AtomicUsize>,
    #[derivative(Default(value = "Arc::new(AtomicUsize::new(0))"))]
    stream_message_count: Arc<AtomicUsize>,
}

impl MockIsolateRpcService {
    pub fn unary_call_count(&self) -> usize {
        self.unary_call_count.load(Ordering::SeqCst)
    }

    pub fn stream_call_count(&self) -> usize {
        self.stream_call_count.load(Ordering::SeqCst)
    }

    pub fn stream_message_count(&self) -> usize {
        self.stream_message_count.load(Ordering::SeqCst)
    }
}

#[tonic::async_trait]
impl IsolateRpcService for MockIsolateRpcService {
    async fn unary_rpc_handler(
        &self,
        method_name: &str,
        request_bytes: &[u8],
    ) -> Result<InvokeIsolateResponse, Status> {
        if method_name != "mock_method" {
            return Err(Status::invalid_argument("Invalid method name"));
        }
        self.unary_call_count.fetch_add(1, Ordering::SeqCst);
        let resp = InvokeIsolateResponse {
            isolate_output: Some(enforcer_proto::enforcer::v1::EzPayloadData {
                datagrams: vec![request_bytes.to_vec()],
            }),
            ..Default::default()
        };
        Ok(resp)
    }

    async fn streaming_rpc_handler(
        &self,
        method_name: &str,
        request: Request<PeekableInvokeIsolateRequestStream>,
    ) -> Result<Response<PinBoxInvokeIsolateResponseStream>, Status> {
        if method_name != "mock_method" {
            return Err(Status::invalid_argument("Invalid method name"));
        }
        self.stream_call_count.fetch_add(1, Ordering::SeqCst);

        let stream_message_count = self.stream_message_count.clone();
        let output_stream = request.into_inner().map(move |req| {
            stream_message_count.fetch_add(1, Ordering::SeqCst);
            let req = req.expect("Request should be Ok");
            let resp = InvokeIsolateResponse {
                isolate_output: req.isolate_input.clone(),
                ..Default::default()
            };
            Ok(resp)
        });

        Ok(Response::new(Box::pin(output_stream)))
    }

    fn service_name(&self) -> &str {
        "mock_service"
    }
}

#[derive(Debug)]
pub struct ErrorIsolateRpcService;

#[tonic::async_trait]
impl IsolateRpcService for ErrorIsolateRpcService {
    async fn unary_rpc_handler(
        &self,
        _method_name: &str,
        _request_bytes: &[u8],
    ) -> Result<InvokeIsolateResponse, Status> {
        Err(Status::unimplemented("Internal error"))
    }

    async fn streaming_rpc_handler(
        &self,
        _method_name: &str,
        _request: Request<PeekableInvokeIsolateRequestStream>,
    ) -> Result<Response<PinBoxInvokeIsolateResponseStream>, Status> {
        Err(Status::unimplemented("Internal error"))
    }

    fn service_name(&self) -> &str {
        "error_service"
    }
}

#[derive(Debug, Derivative, Clone)]
#[derivative(Default)]
pub struct MockIsolateEzBridgeServer {
    #[derivative(Default(value = "Arc::new(AtomicUsize::new(0))"))]
    unary_call_count: Arc<AtomicUsize>,
    #[derivative(Default(value = "Arc::new(AtomicUsize::new(0))"))]
    stream_call_count: Arc<AtomicUsize>,
    #[derivative(Default(value = "Arc::new(AtomicUsize::new(0))"))]
    stream_message_count: Arc<AtomicUsize>,
    #[derivative(Default(value = "Arc::new(Mutex::new(None))"))]
    last_known_state: Arc<Mutex<Option<i32>>>,
}

impl MockIsolateEzBridgeServer {
    pub fn unary_call_count(&self) -> usize {
        self.unary_call_count.load(Ordering::SeqCst)
    }

    pub fn stream_call_count(&self) -> usize {
        self.stream_call_count.load(Ordering::SeqCst)
    }

    pub fn stream_message_count(&self) -> usize {
        self.stream_message_count.load(Ordering::SeqCst)
    }

    pub async fn last_known_state(&self) -> Option<i32> {
        *self.last_known_state.lock().await
    }
}

#[tonic::async_trait]
impl IsolateEzBridge for MockIsolateEzBridgeServer {
    async fn invoke_ez(
        &self,
        request: Request<InvokeEzRequest>,
    ) -> Result<Response<InvokeEzResponse>, Status> {
        let req = request.into_inner();
        self.unary_call_count.fetch_add(1, Ordering::SeqCst);

        let resp = validate_and_process_invoke_ez(req, "mock_")?;
        Ok(Response::new(resp))
    }

    type StreamInvokeEzStream = Pin<Box<dyn GrpcResponseStream<InvokeEzResponse>>>;
    async fn stream_invoke_ez(
        &self,
        request: Request<Streaming<InvokeEzRequest>>,
    ) -> Result<Response<Self::StreamInvokeEzStream>, Status> {
        self.stream_call_count.fetch_add(1, Ordering::SeqCst);
        let stream_message_count = self.stream_message_count.clone();

        let output_stream = request.into_inner().map(move |req| {
            stream_message_count.fetch_add(1, Ordering::SeqCst);
            let Ok(req) = req else {
                return Err(Status::invalid_argument("Request should be Ok"));
            };

            validate_and_process_invoke_ez(req, "stream_mock_")
        });

        Ok(Response::new(Box::pin(output_stream)))
    }

    type CreateMemshareStream = Pin<Box<dyn GrpcResponseStream<CreateMemshareResponse>>>;
    async fn create_memshare(
        &self,
        _request: Request<Streaming<CreateMemshareRequest>>,
    ) -> Result<Response<Self::CreateMemshareStream>, Status> {
        Err(Status::unimplemented("unimplemented"))
    }

    type NotifyIsolateStateStream = Pin<Box<dyn GrpcResponseStream<NotifyIsolateStateResponse>>>;
    async fn notify_isolate_state(
        &self,
        request: Request<Streaming<NotifyIsolateStateRequest>>,
    ) -> Result<Response<Self::NotifyIsolateStateStream>, Status> {
        let last_known_state = self.last_known_state.clone();
        let output_stream = request.into_inner().then(move |req| {
            let last_known_state = last_known_state.clone();
            async move {
                let req = req.expect("Request should be Ok");
                let mut state = last_known_state.lock().await;
                *state = Some(req.new_isolate_state);
                Ok(NotifyIsolateStateResponse::default())
            }
        });
        Ok(Response::new(Box::pin(output_stream)))
    }

    async fn poll_isolate_state(
        &self,
        _request: Request<PollIsolateStateRequest>,
    ) -> Result<Response<PollIsolateStateResponse>, Status> {
        let state = self.last_known_state.lock().await;

        Ok(Response::new(PollIsolateStateResponse { isolate_state: state.unwrap_or(0) }))
    }
}

fn validate_and_process_invoke_ez(
    req: InvokeEzRequest,
    prefix: &str,
) -> Result<InvokeEzResponse, Status> {
    let Some(control_plane_metadata) = req.control_plane_metadata else {
        return Err(Status::invalid_argument("Control plane metadata is required"));
    };

    if !control_plane_metadata.requester_is_local {
        return Err(Status::invalid_argument("Requester is not local"));
    }
    if let Some(payload_scope) = req.isolate_request_iscope {
        if payload_scope.datagram_iscopes.len() != 1 {
            return Err(Status::invalid_argument("Datagram scopes are required"));
        }
        let datagram_scope = &payload_scope.datagram_iscopes[0];
        if datagram_scope.scope_type != DataScopeType::UserPrivate as i32 {
            return Err(Status::invalid_argument("Datagram id is required"));
        }
    } else {
        return Err(Status::invalid_argument("Payload scope is required"));
    }
    Ok(InvokeEzResponse {
        control_plane_metadata: Some(ControlPlaneMetadata {
            ipc_message_id: control_plane_metadata.ipc_message_id,
            destination_ez_instance_id: format!("{}instance_id", prefix),
            destination_service_name: format!("{}service_name", prefix),
            destination_method_name: format!("{}method_name", prefix),
            destination_operator_domain: format!("{}operator_domain", prefix),
            responder_is_local: true,
            ..Default::default()
        }),
        ez_response_payload: req.isolate_request_payload,
        ..Default::default()
    })
}
