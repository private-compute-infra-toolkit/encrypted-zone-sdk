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

use crate::{PeekableInvokeIsolateRequestStream, PinBoxInvokeIsolateResponseStream};
use async_stream::stream;
use derivative::Derivative;
use enforcer_proto::enforcer::v1::ez_isolate_bridge_server::EzIsolateBridge;
use enforcer_proto::enforcer::v1::{
    ControlPlaneMetadata, InvokeIsolateRequest, InvokeIsolateResponse, IsolateState, IsolateStatus,
    UpdateIsolateStateRequest, UpdateIsolateStateResponse,
};
use std::collections::HashMap;
use std::fmt;
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::Arc;
use tokio::sync::RwLock;
use tokio_stream::StreamExt;
use tonic::{Code, Request, Response, Status, Streaming};

/// A trait for services that run within the isolate and handle RPC calls.
///
/// Implementers of this trait are responsible for processing unary and streaming RPC requests
/// that are forwarded by the `RpcDispatcher`.
#[tonic::async_trait]
pub trait IsolateRpcService: Send + Sync + 'static {
    /// Handles an incoming unary RPC call.
    ///
    /// # Arguments
    ///
    /// * `method_name` - The name of the method to invoke.
    /// * `request_bytes` - The serialized request payload.
    ///
    /// # Returns
    ///
    /// Returns a `Result` containing the `InvokeIsolateResponse` or a `tonic::Status` error.
    async fn unary_rpc_handler(
        &self,
        method_name: &str,
        request_bytes: &[u8],
    ) -> Result<InvokeIsolateResponse, Status>;

    /// Handles an incoming streaming RPC call.
    ///
    /// # Arguments
    ///
    /// * `method_name` - The name of the method to invoke.
    /// * `request_stream` - The stream of incoming requests.
    ///
    /// # Returns
    ///
    /// Returns a `Result` containing the response stream or a `tonic::Status` error.
    async fn streaming_rpc_handler(
        &self,
        method_name: &str,
        request_stream: Request<PeekableInvokeIsolateRequestStream>,
    ) -> Result<Response<PinBoxInvokeIsolateResponseStream>, Status>;

    /// Returns the gRPC service name.
    ///
    /// This name is used by the `RpcDispatcher` to route requests to the correct service.
    fn service_name(&self) -> &str;
}

impl fmt::Debug for dyn IsolateRpcService {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "IsolateRpcService {{ service_name: {} }}", self.service_name())
    }
}

/// Dispatches incoming RPC calls to the appropriate `IsolateRpcService`.
///
/// This struct maintains a map of registered services and routes requests based on the
/// `service_name` specified in the `ControlPlaneMetadata`.
#[derive(Debug, Derivative)]
#[derivative(Default, Clone(bound = ""))]
pub struct RpcDispatcher {
    #[derivative(Default(value = "Arc::new(RwLock::new(HashMap::new()))"))]
    service_map: Arc<RwLock<HashMap<String, Arc<dyn IsolateRpcService>>>>,
    #[derivative(Default(value = "Arc::new(AtomicI32::new(IsolateState::Ready as i32))"))]
    current_state: Arc<AtomicI32>,
}

impl RpcDispatcher {
    /// Creates a new `RpcDispatcher` with an initial service.
    ///
    /// # Arguments
    ///
    /// * `initial_service` - The first service to register with the dispatcher.
    pub fn new(initial_service: Arc<dyn IsolateRpcService>) -> Self {
        let mut service_map = HashMap::new();
        service_map.insert(initial_service.service_name().to_string(), initial_service);
        Self {
            service_map: Arc::new(RwLock::new(service_map)),
            // Have this start at Ready since the Isolate already notified the Enforcer
            // that it is Ready before the dispatcher is used.
            current_state: Arc::new(AtomicI32::new(IsolateState::Ready as i32)),
        }
    }

    /// Adds a new service to the dispatcher.
    ///
    /// This method allows dynamically registering additional services after the dispatcher
    /// has been created.
    ///
    /// # Arguments
    ///
    /// * `service` - The service to add.
    pub async fn add_service(&self, service: Arc<dyn IsolateRpcService>) {
        let mut service_map = self.service_map.write().await;
        service_map.insert(service.service_name().to_string(), service);
    }

    async fn resolve_service(
        &self,
        control_plane_metadata: Option<ControlPlaneMetadata>,
    ) -> Result<
        (ControlPlaneMetadata, Arc<dyn IsolateRpcService>),
        Result<InvokeIsolateResponse, Status>,
    > {
        let metadata = match control_plane_metadata {
            Some(m) => m,
            None => {
                return Err(get_invalid_isolate_response(
                    "Missing control plane metadata".to_string(),
                    Code::InvalidArgument,
                    None,
                ));
            }
        };

        let ipc_message_id = metadata.ipc_message_id;
        let service_name = &metadata.destination_service_name;

        let current_state = self.current_state.load(Ordering::Acquire);
        if current_state != IsolateState::Ready as i32 {
            return Err(get_invalid_isolate_response(
                format!("Isolate is not ready. Current state: {}", current_state),
                Code::FailedPrecondition,
                Some(ipc_message_id),
            ));
        }

        match self.service_map.read().await.get(service_name) {
            Some(s) => Ok((metadata, s.clone())),
            None => Err(get_invalid_isolate_response(
                format!("Service not supported by isolate: {}", service_name),
                Code::NotFound,
                Some(ipc_message_id),
            )),
        }
    }
}

#[tonic::async_trait]
impl EzIsolateBridge for RpcDispatcher {
    // Unary
    async fn invoke_isolate(
        &self,
        request: Request<InvokeIsolateRequest>,
    ) -> Result<Response<InvokeIsolateResponse>, Status> {
        let req = request.into_inner();

        let (metadata, service) = match self.resolve_service(req.control_plane_metadata).await {
            Ok(res) => res,
            Err(e) => return e.map(Response::new),
        };

        let ipc_message_id = metadata.ipc_message_id;
        let method_name = &metadata.destination_method_name;
        log::info!(
            "Received unary request for method {} ipc_message_id {} service_name {}",
            method_name,
            ipc_message_id,
            service.service_name()
        );

        // TODO: Support all datagram bytes in the payload.
        let request_bytes = match req.isolate_input.and_then(|i| i.datagrams.into_iter().next()) {
            Some(d) => d,
            None => {
                return get_invalid_isolate_response(
                    "Missing datagram in request payload".to_string(),
                    Code::InvalidArgument,
                    Some(ipc_message_id),
                )
                .map(Response::new);
            }
        };

        match service.unary_rpc_handler(method_name, &request_bytes).await {
            Ok(mut response) => {
                let metadata = response.control_plane_metadata.get_or_insert_with(Default::default);
                metadata.ipc_message_id = ipc_message_id;
                metadata.responder_is_local = true;
                Ok(Response::new(response))
            }
            Err(e) => get_invalid_isolate_response(
                e.message().to_string(),
                e.code(),
                Some(ipc_message_id),
            )
            .map(Response::new),
        }
    }

    type StreamInvokeIsolateStream = PinBoxInvokeIsolateResponseStream;
    async fn stream_invoke_isolate(
        &self,
        request_stream: Request<Streaming<InvokeIsolateRequest>>,
    ) -> Result<Response<Self::StreamInvokeIsolateStream>, Status> {
        let this = self.clone();
        let output_stream = stream! {
          let mut peekable_stream = request_stream.into_inner().peekable();
          let (metadata, service) = if let Some(first_req) =  peekable_stream.peek().await {
              match first_req {
                Ok(req) => {
                     match this.resolve_service(req.control_plane_metadata.clone()).await {
                        Ok(res) => res,
                        Err(e) => {
                             yield e;
                             return;
                        }
                     }
                }
                Err(status) => {
                    yield get_invalid_isolate_response(
                        status.message().to_string(),
                        status.code(),
                        None,
                    );
                    return;
                }
              }
          } else {
             yield get_invalid_isolate_response(
                 "Stream ended unexpectedly".to_string(),
                 Code::InvalidArgument,
                 None,
             );
            return;
          };

          let ipc_message_id = metadata.ipc_message_id;
          let method_name = &metadata.destination_method_name;

          log::info!(
            "Received stream request for method {} ipc_message_id {} service_name {}",
            method_name,
            ipc_message_id,
            service.service_name()
          );

          // Delegate to the resolved service.
          // Note: The first message (peeked above) is logically consumed from the stream
          // passed to the handler if the Peekable internal buffer is discarded.
          // However, we now pass the peekable stream itself (wrapped in Request), so no data is lost.
          let response_stream = service
                .streaming_rpc_handler(method_name, Request::new(peekable_stream))
                .await?
                .into_inner();
          // `response_stream` is not `Unpin` because we removed the `Unpin` bound from the
          // `InvokeIsolateResponseStream` trait alias to support `async-stream`.
          // `StreamExt::next()` requires the stream to be `Unpin` so it can pin it inside.
          // Since our stream is `!Unpin`, we must manually pin it to the stack here.
          tokio::pin!(response_stream);

          while let Some(mut invoke_isolate_resp) = response_stream.next().await {
            // Attach metadata to successful responses to ensure proper routing back to the caller.
            if let Ok(resp) = &mut invoke_isolate_resp {
                let metadata = resp.control_plane_metadata.get_or_insert_with(Default::default);
                metadata.ipc_message_id = ipc_message_id;
                metadata.responder_is_local = true;
            }
            yield invoke_isolate_resp;
          }
        };

        Ok(Response::new(Box::pin(output_stream)))
    }

    async fn update_isolate_state(
        &self,
        request: Request<UpdateIsolateStateRequest>,
    ) -> Result<Response<UpdateIsolateStateResponse>, Status> {
        let req = request.into_inner();
        let move_to_state =
            IsolateState::try_from(req.move_to_state).unwrap_or(IsolateState::Unspecified);
        log::info!("Received update isolate state request: {:?}", move_to_state);
        let new_state = match move_to_state {
            // The Isolate is responsible for the Starting -> Ready transition.
            // If the bridge is working, then it's safe to say Ready.
            IsolateState::Starting => IsolateState::Ready,
            // For all other transitions (for now), we will let the Enforcer decide.
            _ => move_to_state,
        };
        self.current_state.store(new_state as i32, Ordering::Release);
        Ok(Response::new(UpdateIsolateStateResponse { current_state: new_state as i32 }))
    }
}

fn get_invalid_isolate_response(
    message: String,
    code: Code,
    ipc_message_id: Option<u64>,
) -> Result<InvokeIsolateResponse, Status> {
    Ok(InvokeIsolateResponse {
        status: Some(IsolateStatus { code: code as i32, message }),
        control_plane_metadata: Some(ControlPlaneMetadata {
            ipc_message_id: ipc_message_id.unwrap_or_default(),
            responder_is_local: true,
            ..Default::default()
        }),
        ..Default::default()
    })
}
