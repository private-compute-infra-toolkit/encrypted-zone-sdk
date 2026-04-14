// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef CORE_CPP_TEST_MOCK_ISOLATE_RPC_SERVICE_H
#define CORE_CPP_TEST_MOCK_ISOLATE_RPC_SERVICE_H

#include <gmock/gmock.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/strings/string_view.h"
#include "core/cpp/src/ez_isolate_bridge_server.h"
#include "google/protobuf/repeated_ptr_field.h"

namespace EzIsolateBridgeSdk {

class MockIsolateRpcService final : public IsolateRpcService {
 public:
  MOCK_METHOD(
      void, IsolateRpcMethodHandler,
      (grpc::CallbackServerContext * context, const std::string& method_name,
       const google::protobuf::RepeatedPtrField<std::string>& request_bytes,
       std::string& response_bytes,
       std::vector<std::string>& response_shared_memory_handles,
       std::vector<std::string>& response_fileshare_handles,
       absl::AnyInvocable<void(grpc::Status) &&> done),
      (override));
  MOCK_METHOD(
      grpc::Status, IsolateStreamRpcMethodHandler,
      (uintptr_t stream_id, const InvokeIsolateRequest& request,
       ResponseWriter* response_writer,
       std::shared_ptr<std::vector<std::string>> response_shared_memory_handles,
       std::shared_ptr<std::vector<std::string>> response_fileshare_handles),
      (override));
  MOCK_METHOD(grpc::Status, ForwardStreamingMessage,
              (uintptr_t stream_id, const InvokeIsolateRequest& request),
              (override));
  MOCK_METHOD(grpc::Status, RequestStreamClosed, (uintptr_t stream_id),
              (override));
  MOCK_METHOD(grpc::Status, OnFileshareEvent,
              (std::string_view share_handle,
               enforcer::v1::FileshareEvent::FileshareEventType event_type),
              (override));
  MOCK_METHOD(absl::string_view, GetServiceName, (), (override));
};

}  // namespace EzIsolateBridgeSdk

#endif  // CORE_CPP_TEST_MOCK_ISOLATE_RPC_SERVICE_H
