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
use enforcer_proto::enforcer::v1::{EzPayloadData, InvokeIsolateRequest};
use prost::Message;
use rust_core::{
    invoke_isolate_stream_to_message_stream, message_stream_to_invoke_isolate_stream,
    message_to_invoke_isolate_response,
};
use test_message_proto::test_message::TestMessage;
use tokio_stream::StreamExt;
use tonic::Request;

#[test]
fn test_message_to_invoke_isolate_response() {
    let message = TestMessage { field1: "hello".to_string(), field2: "world".to_string() };
    let scope = DataScopeType::UserPrivate;

    let response = message_to_invoke_isolate_response(message.clone(), scope);

    // Verify scope
    let output_scope = response.isolate_output_iscope.expect("should have scope");
    assert_eq!(output_scope.datagram_iscopes.len(), 1);
    assert_eq!(output_scope.datagram_iscopes[0].scope_type, scope as i32);

    // Verify payload
    let output_data = response.isolate_output.expect("should have output");
    assert_eq!(output_data.datagrams.len(), 1);
    let decoded_message =
        TestMessage::decode(output_data.datagrams[0].as_slice()).expect("should decode");
    assert_eq!(decoded_message.field1, message.field1);
    assert_eq!(decoded_message.field2, message.field2);

    // Verify status
    let status = response.status.expect("should have status");
    assert_eq!(status.code, 0);
    assert_eq!(status.message, "OK");
}

#[tokio::test]
async fn test_invoke_isolate_stream_to_message_stream() {
    let message1 = TestMessage { field1: "one".to_string(), field2: "1".to_string() };
    let message2 = TestMessage { field1: "two".to_string(), field2: "2".to_string() };

    let messages = vec![message1.clone(), message2.clone()];

    let invoke_requests = messages.into_iter().map(|msg| {
        Ok(InvokeIsolateRequest {
            isolate_input: Some(EzPayloadData { datagrams: vec![msg.encode_to_vec()] }),
            ..Default::default()
        })
    });

    let request_stream = Request::new(tokio_stream::iter(invoke_requests));

    let mut message_stream = invoke_isolate_stream_to_message_stream::<TestMessage>(request_stream);

    let mut received_messages = Vec::new();
    while let Some(msg_result) = message_stream.next().await {
        received_messages.push(msg_result.expect("should be ok"));
    }

    assert_eq!(received_messages.len(), 2);
    assert_eq!(received_messages[0].field1, message1.field1);
    assert_eq!(received_messages[1].field1, message2.field1);
}

#[tokio::test]
async fn test_message_stream_to_invoke_isolate_stream() {
    let message1 = TestMessage { field1: "one".to_string(), field2: "1".to_string() };
    let message2 = TestMessage { field1: "two".to_string(), field2: "2".to_string() };

    let messages = vec![Ok(message1.clone()), Ok(message2.clone())];
    let message_stream = tokio_stream::iter(messages);
    // We need to map to Result<T, Status> explicitly if not already, but our vec is Result<T, Status>
    // Actually, `message_stream` needs to be valid GrpcRequestStream<T>.
    // GrpcRequestStream<T> = Stream<Item = Result<T, Status>> + Send + Unpin + 'static.
    // tokio_stream::iter returns a stream that is Unpin.

    // Explicitly annotate type for stream to satisfy trait bounds if needed, or let inference work.
    // But `tokio_stream::iter` produces an iterator stream.
    // We need to type inference to work for `impl GrpcRequestStream<T>`.

    let scope = DataScopeType::UserPrivate;
    let mut response_stream = message_stream_to_invoke_isolate_stream(message_stream, scope);

    let mut received_responses = Vec::new();
    while let Some(resp_result) = response_stream.next().await {
        received_responses.push(resp_result.expect("should be ok"));
    }

    assert_eq!(received_responses.len(), 2);

    // Check first response
    let resp1 = &received_responses[0];
    let decoded1 =
        TestMessage::decode(resp1.isolate_output.as_ref().unwrap().datagrams[0].as_slice())
            .unwrap();
    assert_eq!(decoded1.field1, message1.field1);
    assert_eq!(
        resp1.isolate_output_iscope.as_ref().unwrap().datagram_iscopes[0].scope_type,
        scope as i32
    );
}

#[tokio::test]
async fn test_invoke_isolate_stream_to_message_stream_error() {
    // Test case where datagram is missing
    let invoke_requests = vec![Ok(InvokeIsolateRequest::default())];
    let request_stream = Request::new(tokio_stream::iter(invoke_requests));

    let mut message_stream = invoke_isolate_stream_to_message_stream::<TestMessage>(request_stream);

    let result = message_stream.next().await.unwrap();
    assert!(result.is_err());
    assert_eq!(result.unwrap_err().code(), tonic::Code::InvalidArgument);
}
