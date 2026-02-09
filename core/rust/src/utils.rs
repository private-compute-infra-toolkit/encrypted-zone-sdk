// Copyright 2025 Google LLC
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

use enforcer_proto::data_scope_proto::enforcer::v1::DataScopeType;
use enforcer_proto::enforcer::v1::{
    EzPayloadData, EzPayloadIsolateScope, InvokeIsolateRequest, InvokeIsolateResponse,
    IsolateDataScope, IsolateStatus,
};
use futures::Stream;
use prost::Message;
use std::pin::Pin;
use tokio_stream::adapters::Peekable;
use tokio_stream::StreamExt;
use tonic::{Request, Status, Streaming};
use trait_set::trait_set;

trait_set! {
    // We define a generic stream trait for any message T
    pub trait GrpcRequestStream<T: Message> = Stream<Item = Result<T, Status>> + Send + Unpin + 'static;
    pub trait GrpcClientRequestStream<T: Message> = Stream<Item = T> + Send + Unpin + 'static;
    pub trait GrpcResponseStream<T: Message> = Stream<Item = Result<T, Status>> + Send  + 'static;
    /// A stream of `InvokeIsolateRequest` messages.
    pub trait InvokeIsolateRequestStream = GrpcRequestStream<InvokeIsolateRequest>;
    /// A stream of `InvokeIsolateResponse` messages.
    pub trait InvokeIsolateResponseStream = GrpcResponseStream<InvokeIsolateResponse>;
}

// This type alias bound by Message trait is primarily for readability.
// It is important to note that bounds on generic parameters in type aliases are not currently enforced by the Rust type checker.
// This is a known limitation that may be addressed in a future edition of Rust.
// For more information, please refer to issue https://github.com/rust-lang/rust/issues/112792.
#[allow(type_alias_bounds)]
pub type PinBoxGrpcResponseStream<T: Message> = Pin<Box<dyn GrpcResponseStream<T>>>;
pub type PinBoxInvokeIsolateResponseStream = Pin<Box<dyn InvokeIsolateResponseStream>>;
pub type PeekableInvokeIsolateRequestStream = Peekable<Streaming<InvokeIsolateRequest>>;

/// Wraps a generic Protobuf message into an `InvokeIsolateResponse`.
///
/// This function serializes the message and sets the appropriate isolation scope metadata.
///
/// # Arguments
///
/// * `message` - The message to wrap.
/// * `response_scope` - The data scope to assign to the response.
///
/// # Returns
///
/// Returns an `InvokeIsolateResponse` containing the serialized message.
pub fn message_to_invoke_isolate_response<T: Message>(
    message: T,
    response_scope: DataScopeType,
) -> InvokeIsolateResponse {
    InvokeIsolateResponse {
        control_plane_metadata: Default::default(),
        isolate_output_iscope: Some(EzPayloadIsolateScope {
            datagram_iscopes: vec![IsolateDataScope {
                scope_type: response_scope as i32,
                ..Default::default()
            }],
        }),
        isolate_output: Some(EzPayloadData { datagrams: vec![message.encode_to_vec()] }),
        status: Some(IsolateStatus { code: 0, message: "OK".to_string() }),
    }
}

/// Converts a raw `InvokeIsolateRequest` stream into a typed message stream.
///
/// This helper extracts the payload from each `InvokeIsolateRequest`, decodes it into
/// message type `T`, and returns a stream of these decoded messages.
///
/// # Arguments
///
/// * `request_stream` - The raw gRPC request stream.
///
/// # Returns
///
/// Returns a pinned, boxed stream of `Result<T, Status>`.
pub fn invoke_isolate_stream_to_message_stream<T: Message + Default>(
    request_stream: Request<impl InvokeIsolateRequestStream>,
) -> PinBoxGrpcResponseStream<T> {
    let mapped_stream = request_stream.into_inner().map(|req_result| {
        let req = req_result?;
        // TODO: Support all datagram bytes in the payload.
        let request_bytes = match req.isolate_input.and_then(|i| i.datagrams.into_iter().next()) {
            Some(d) => d,
            None => {
                return Err(Status::invalid_argument("Missing datagram in request payload"));
            }
        };
        T::decode(request_bytes.as_slice()).map_err(|e| Status::internal(e.to_string()))
    });
    Box::pin(mapped_stream)
}

/// Converts a typed message stream into an `InvokeIsolateResponse` stream.
///
/// This helper wraps each message in the input stream into an `InvokeIsolateResponse`
/// with the specified `request_scope` and returns the resulting stream.
///
/// # Arguments
///
/// * `message_stream` - The stream of typed messages.
/// * `request_scope` - The data scope to assign to each response.
///
/// # Returns
///
/// Returns a pinned, boxed stream of `Result<InvokeIsolateResponse, Status>`.
pub fn message_stream_to_invoke_isolate_stream<T: Message>(
    message_stream: impl GrpcRequestStream<T>,
    request_scope: DataScopeType,
) -> PinBoxInvokeIsolateResponseStream {
    let mapped_stream = message_stream.map(move |message_result| {
        let message = message_result?;
        Ok(message_to_invoke_isolate_response(message, request_scope))
    });
    Box::pin(mapped_stream)
}

/// Creates an `IsolateRpcServer` with the specified services.
///
/// This macro simplifies the setup of the server by taking care of:
/// 1. Creating the service adapters.
/// 2. Initializing the `RpcDispatcher`.
/// 3. Registering all provided services.
///
/// # Arguments
///
/// * `ServiceType => service_logic` - A list of key-value pairs where the key is the
///   Isolate RPC Service adapter type and the value is the service logic implementation.
///   The first pair is used to initialize the dispatcher, and subsequent pairs are added
///   dynamically.
///
/// # Returns
///
/// Returns a new `IsolateRpcServer` instance.
///
/// # Example
///
/// ```rust,ignore
/// let server = create_isolate_server! {
///     GreeterIsolateRpcService => greeter_logic,
///     OtherIsolateRpcService => other_logic
/// };
/// ```
///
/// # Note
///
/// This macro generates async code (specifically `dispatcher.add_service(...).await`),
/// so it **must** be called within an `async` block or function.
#[macro_export]
macro_rules! create_isolate_server {
    ($fk:path => $fv:expr $(, $rk:path => $rv:expr)* $(,)?) => {
        {
            let first_service = std::sync::Arc::new({
                use $fk as Service;
                Service::new($fv)
            });
            let dispatcher = std::sync::Arc::new($crate::RpcDispatcher::new(first_service));
            $(
                dispatcher.add_service(std::sync::Arc::new({
                    use $rk as Service;
                    Service::new($rv)
                })).await;
            )*
            $crate::IsolateRpcServer::new(dispatcher)
        }
    };
}

/// Creates an `IsolateRpcServer` and sets the client on the service logic.
///
/// This macro wraps `create_isolate_server!`, but also assumes that the provided service logic
/// expressions are clones of an `Arc` (or similar) and attempts to set the SDK client on them
/// after the server is created.
///
/// # Arguments
///
/// * `ServiceType => service_logic` - Same as `create_isolate_server!`.
///
/// # Example
///
/// ```rust,ignore
/// let server = create_isolate_server_with_client! {
///     GreeterIsolateRpcService => greeter_logic,
///     OtherIsolateRpcService => other_logic
/// };
/// ```
#[macro_export]
macro_rules! create_isolate_server_with_client {
    ($fk:path => $fv:expr $(, $rk:path => $rv:expr)* $(,)?) => {
        {
            let server = $crate::create_isolate_server! {
                $fk => $fv.clone()
                $(, $rk => $rv.clone())*
            };
            let client = server.get_isolate_ez_bridge_client().await;
            $fv.set_client(client.clone());
            $(
                $rv.set_client(client.clone());
            )*
            server
        }
    };
}

/// Creates an `IsolateRpcServer` with the specified services and response scopes.
///
/// This macro simplifies the setup of the server by taking care of:
/// 1. Creating the service adapters.
/// 2. Initializing the `RpcDispatcher`.
/// 3. Registering all provided services.
///
/// # Arguments
///
/// * `ServiceType => service_logic => scope` - A list of key-value pairs where the key is the
///   Isolate RPC Service adapter type, the second element is the service logic implementation,
///   and the third element is the `DataScopeType`.
///   The first pair is used to initialize the dispatcher, and subsequent pairs are added
///   dynamically.
///
/// # Returns
///
/// Returns a new `IsolateRpcServer` instance.
///
/// # Example
///
/// ```rust,ignore
/// let server = create_isolate_server_with_resp_scope! {
///     GreeterIsolateRpcService => greeter_logic => DataScopeType::Public,
///     OtherIsolateRpcService => other_logic => DataScopeType::UserPrivate
/// };
/// ```
///
/// # Note
///
/// This macro generates async code (specifically `dispatcher.add_service(...).await`),
/// so it **must** be called within an `async` block or function.
#[macro_export]
macro_rules! create_isolate_server_with_resp_scope {
    ($fk:path => $fv:expr => $fscope:expr $(, $rk:path => $rv:expr => $rscope:expr)* $(,)?) => {
        {
            let first_service = std::sync::Arc::new({
                use $fk as Service;
                Service::new_with_scope($fv, $fscope)
            });
            let dispatcher = std::sync::Arc::new($crate::RpcDispatcher::new(first_service));
            $(
                dispatcher.add_service(std::sync::Arc::new({
                    use $rk as Service;
                    Service::new_with_scope($rv, $rscope)
                })).await;
            )*
            $crate::IsolateRpcServer::new(dispatcher)
        }
    };
}

/// Creates an `IsolateRpcServer` with the specified services and response scopes, and sets the client on the service logic.
///
/// This macro wraps `create_isolate_server_with_resp_scope!`, but also assumes that the provided service logic
/// expressions are clones of an `Arc` (or similar) and attempts to set the SDK client on them
/// after the server is created.
///
/// # Arguments
///
/// * `ServiceType => service_logic => scope` - Same as `create_isolate_server_with_resp_scope!`.
///
/// # Example
///
/// ```rust,ignore
/// let server = create_isolate_server_with_resp_scope_and_client! {
///     GreeterIsolateRpcService => greeter_logic => DataScopeType::Public,
///     OtherIsolateRpcService => other_logic => DataScopeType::UserPrivate
/// };
/// ```
#[macro_export]
macro_rules! create_isolate_server_with_resp_scope_and_client {
    ($fk:path => $fv:expr => $fscope:expr $(, $rk:path => $rv:expr => $rscope:expr)* $(,)?) => {
        {
            let server = $crate::create_isolate_server_with_resp_scope! {
                $fk => $fv.clone() => $fscope
                $(, $rk => $rv.clone() => $rscope)*
            };
            let client = server.get_isolate_ez_bridge_client().await;
            $fv.set_client(client.clone());
            $(
                $rv.set_client(client.clone());
            )*
            server
        }
    };
}
