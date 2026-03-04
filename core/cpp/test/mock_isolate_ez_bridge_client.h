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

#ifndef MOCK_ISOLATE_EZ_BRIDGE_CLIENT_H
#define MOCK_ISOLATE_EZ_BRIDGE_CLIENT_H

#include <gmock/gmock.h>
#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/sync_stream.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "absl/functional/any_invocable.h"
#include "core/cpp/src/isolate_ez_bridge_client.h"
#include "core/cpp/src/mem_share_response.h"

namespace IsolateEzBridgeSdk {

class MockBridgeClient : public IsolateEzBridgeClient {
 public:
  MOCK_METHOD(void, RegisterLocalHandler,
              (const std::string& service_name,
               std::function<grpc::Status(const std::string& method_name,
                                          const std::string& request,
                                          std::string& response)>
                   handler),
              (override));
  MOCK_METHOD(grpc::Status, IsolateRpcCall,
              (grpc::ClientContext * context,
               const std::string& operator_domain,
               const std::string& service_name, const std::string& method_name,
               const std::string& ez_instance_id,
               const std::string& request_bytes, std::string& response_bytes),
              (override));
  MOCK_METHOD(
      (std::unique_ptr<::grpc::ClientReaderWriter<
           ::enforcer::v1::InvokeEzRequest, ::enforcer::v1::InvokeEzResponse>>),
      IsolateStreamRpcCall,
      (grpc::ClientContext * context, const std::string& operator_domain,
       const std::string& service_name, const std::string& method_name,
       const std::string& ez_instance_id),
      (override));
  MOCK_METHOD(bool, NewIsolateState,
              (::enforcer::v1::IsolateState new_isolate_state), (override));
  MOCK_METHOD(MemShareResponse, CreateMemShareInternal,
              (int64_t size, char** ptr), (override));
  MOCK_METHOD(IsolateEzBridgeSdk::Vec<char>, ReceiveMemShareInternal, (),
              (override));
  MOCK_METHOD(void, AsyncIsolateRpcCall,
              (grpc::ClientContext * context,
               const std::string& operator_domain,
               const std::string& service_name, const std::string& method_name,
               const std::string& ez_instance_id,
               const std::string& request_bytes, std::string* response_bytes,
               absl::AnyInvocable<void(grpc::Status) &&> callback),
              (override));
};

}  // namespace IsolateEzBridgeSdk

#endif  // MOCK_ISOLATE_EZ_BRIDGE_CLIENT_H
