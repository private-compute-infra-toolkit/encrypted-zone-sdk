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

use crate::isolate_ez_bridge_client::IsolateEzBridgeSdkClient;
use crate::{GrpcClientRequestStream, PinBoxGrpcResponseStream};
use enforcer_proto::data_scope_proto::enforcer::v1::DataScopeType;
use enforcer_proto::enforcer::v1::{
    ControlPlaneMetadata, EzPayloadData, EzPayloadIsolateScope, InvokeEzRequest, InvokeEzResponse,
    IsolateDataScope,
};
use prost::Message;
use std::sync::Arc;
use tokio_stream::StreamExt;
use tonic::{Request, Response, Status};

/// Handles RPC calls to the isolate by wrapping the `IsolateEzBridgeSdkClient`.
///
/// This struct is responsible for marshaling requests and responses between the client application
/// and the Encrypted Zone (EZ) isolate. It handles the details of creating `InvokeEzRequest` messages,
/// managing IPC message IDs, and decoding `InvokeEzResponse` messages.
#[derive(Debug, Clone)]
pub struct RpcHandler {
    client: Arc<IsolateEzBridgeSdkClient>,
    operator_domain: String,
    service_name: String,
    request_scope: DataScopeType,
    // TODO: Add shared memory management.
}

impl RpcHandler {
    /// Creates a new `RpcHandler`.
    ///
    /// # Arguments
    ///
    /// * `client` - The shared `IsolateEzBridgeSdkClient` used to communicate with the bridge.
    /// * `operator_domain` - The domain of the service running in the isolate.
    /// * `service_name` - The name of the service running in the isolate.
    /// * `request_scope` - The data scope to use for requests.
    pub fn new(
        client: Arc<IsolateEzBridgeSdkClient>,
        operator_domain: String,
        service_name: String,
        request_scope: DataScopeType,
    ) -> Self {
        Self { client, operator_domain, service_name, request_scope }
    }

    fn create_invoke_ez_request(
        &self,
        ipc_message_id: u64,
        method_name: String,
        request_bytes: Vec<u8>,
    ) -> InvokeEzRequest {
        InvokeEzRequest {
            control_plane_metadata: Some(ControlPlaneMetadata {
                ipc_message_id,
                requester_is_local: true,
                destination_operator_domain: self.operator_domain.clone(),
                destination_service_name: self.service_name.clone(),
                destination_method_name: method_name,
                ..Default::default()
            }),
            isolate_request_iscope: Some(EzPayloadIsolateScope {
                datagram_iscopes: vec![IsolateDataScope {
                    scope_type: self.request_scope as i32,
                    ..Default::default()
                }],
            }),
            isolate_request_payload: Some(EzPayloadData { datagrams: vec![request_bytes] }),
        }
    }
}

impl RpcHandler {
    /// Performs a unary RPC call to the isolate.
    ///
    /// This method encodes the request message, creates an `InvokeEzRequest`, sends it to the isolate
    /// via the bridge client, and decodes the response.
    ///
    /// # Arguments
    ///
    /// * `method_name` - The name of the RPC method to invoke.
    /// * `request` - The request message.
    ///
    /// # Generic Parameters
    ///
    /// * `T` - The type of the request message (must implement `prost::Message`).
    /// * `U` - The type of the response message (must implement `prost::Message` + `Default`).
    ///
    /// # Returns
    ///
    /// Returns a `Result` containing the decoded response message or a `tonic::Status` error.
    pub async fn isolate_rpc_call<T: Message, U: Message + Default>(
        &self,
        method_name: &str,
        request: T,
    ) -> Result<U, Status> {
        let request_bytes = request.encode_to_vec();
        let ipc_message_id = rand::random::<u64>();

        let request =
            self.create_invoke_ez_request(ipc_message_id, method_name.to_string(), request_bytes);

        let response = self.client.invoke_ez(request).await?;

        if response.control_plane_metadata.as_ref().map_or(0, |m| m.ipc_message_id)
            != ipc_message_id
        {
            log::error!(
                "Error: Mismatched IPC message id. Expected {}, got {:?}",
                ipc_message_id,
                response.control_plane_metadata.as_ref().map(|m| m.ipc_message_id)
            );
            return Err(Status::internal("Mismatched IPC message id"));
        }

        decode_invoke_ez_response(&response)
    }

    /// Performs a streaming RPC call to the isolate.
    ///
    /// This method simplifies the process of creating a bidirectional streaming RPC. It maps the
    /// input stream of requests to `InvokeEzRequest` messages and maps the output stream of
    /// `InvokeEzResponse` messages back to the expected response type.
    ///
    /// # Arguments
    ///
    /// * `method_name` - The name of the RPC method to invoke.
    /// * `request_stream` - The stream of request messages.
    ///
    /// # Generic Parameters
    ///
    /// * `T` - The type of the request message (must implement `prost::Message`).
    /// * `U` - The type of the response message (must implement `prost::Message` + `Default`).
    ///
    /// # Returns
    ///
    /// Returns a `Result` containing the response stream or a `tonic::Status` error.
    pub async fn stream_isolate_rpc_call<T: Message, U: Message + Default>(
        &self,
        method_name: &str,
        request_stream: Request<impl GrpcClientRequestStream<T>>,
    ) -> Result<Response<PinBoxGrpcResponseStream<U>>, Status> {
        let this = self.clone();
        let method_name = method_name.to_string();
        let ipc_message_id = rand::random::<u64>();

        let invoke_ez_stream = request_stream.into_inner().map(move |req| {
            let request_bytes = req.encode_to_vec();
            this.create_invoke_ez_request(ipc_message_id, method_name.clone(), request_bytes)
        });

        let response_stream = self.client.stream_invoke_ez(invoke_ez_stream).await?;
        let mapped_response_stream = response_stream.map(|res| {
            let response = res?;
            decode_invoke_ez_response(&response)
        });

        Ok(Response::new(Box::pin(mapped_response_stream)))
    }
}

fn decode_invoke_ez_response<U: Message + Default>(
    response: &InvokeEzResponse,
) -> Result<U, Status> {
    // TODO: Handle shared memory handles.
    let response_bytes =
        response.ez_response_payload.as_ref().and_then(|p| p.datagrams.first()).ok_or_else(
            || {
                log::error!("Missing response payload");
                Status::internal("Missing response payload")
            },
        )?;

    U::decode(response_bytes.as_slice()).map_err(|e| {
        log::error!("Failed to decode response: {:?}", e);
        Status::internal(e.to_string())
    })
}
