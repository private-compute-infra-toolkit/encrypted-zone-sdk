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

use enforcer_proto::enforcer::v1::ez_public_api_client::EzPublicApiClient;
use enforcer_proto::enforcer::v1::{CallParameters, CallRequest, SessionMetadata};
use prost::Message;
use std::collections::HashMap;
use summation_proto::summation::IntegerSequenceRequest;
use tonic::transport::Channel;

pub async fn ez_api_retryable_connect(
    address: String,
) -> Result<EzPublicApiClient<Channel>, tonic::transport::Error> {
    EzPublicApiClient::connect(address).await
}

pub fn create_call_request_for_summation(
    sum_start: i64,
    sum_end: i64,
    service_name: String,
    method_name: String,
) -> CallRequest {
    CallRequest {
        session_metadata: Some(SessionMetadata {
            session_id: rand::random(),
            ez_client_uuid: vec![],
        }),
        operator_domain: "playground_example".to_string(),
        service_name,
        method_name,
        input_params: Some(CallParameters {
            public_input: create_integer_sequence_request(sum_start, sum_end).encode_to_vec(),
            encrypted_input: vec![],
            request_metadata: HashMap::new(),
        }),
        ..Default::default()
    }
}

fn create_integer_sequence_request(sum_start: i64, sum_end: i64) -> IntegerSequenceRequest {
    IntegerSequenceRequest {
        start_at: sum_start,
        end_at: sum_end,
        expected_result: Some(get_expected_sum_result(sum_start, sum_end)),
    }
}

pub fn get_expected_sum_result(sum_start: i64, sum_end: i64) -> i64 {
    ((sum_end) * (sum_end + 1) - (sum_start - 1) * (sum_start)) / 2
}
