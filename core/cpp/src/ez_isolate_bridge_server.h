/*
 * Copyright 2026 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CORE_CPP_SRC_EZ_ISOLATE_BRIDGE_SERVER_H
#define CORE_CPP_SRC_EZ_ISOLATE_BRIDGE_SERVER_H

#include <grpcpp/server.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/server_callback.h>
#include <grpcpp/support/status.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "absl/flags/declare.h"
#include "absl/functional/any_invocable.h"
#include "enforcer/v1/ez_isolate_bridge.grpc.pb.h"
#include "enforcer/v1/ez_isolate_bridge.pb.h"
#include "enforcer/v1/isolate_ez_bridge.pb.h"
#include "google/protobuf/repeated_ptr_field.h"

namespace EzIsolateBridgeSdk {

using enforcer::v1::EzIsolateBridge;
using enforcer::v1::InvokeIsolateRequest;
using enforcer::v1::InvokeIsolateResponse;

class IsolateRpcService;
void ForwardRequest(grpc::CallbackServerContext* context,
                    std::shared_ptr<IsolateRpcService> isolate_rpc_service,
                    const InvokeIsolateRequest& request,
                    InvokeIsolateResponse& response,
                    absl::AnyInvocable<void(grpc::Status) &&> done);

template <typename T>
std::optional<T> MergeProtobin(
    const google::protobuf::RepeatedPtrField<std::string>& data) {
  T request;
  for (const auto& str : data) {
    if (!request.MergeFromString(str)) {
      return std::nullopt;
    }
  }
  return request;
}

class ResponseWriter {
 public:
  virtual ~ResponseWriter() = default;
  // Call this to write a response back to the client stream.
  virtual void WriteResponse(
      std::unique_ptr<InvokeIsolateResponse> response) = 0;
  // Call this to finish the stream from the client service.
  virtual void FinishStream(const grpc::Status& status) = 0;
};

// Base class for every Isolate RPC Service implementation.
// Implementations should ideally be auto-generated for gRPC-based servers.
class IsolateRpcService {
 public:
  virtual ~IsolateRpcService() = default;
  virtual void IsolateRpcMethodHandler(
      grpc::CallbackServerContext* context, const std::string& method_name,
      const google::protobuf::RepeatedPtrField<std::string>& request_bytes,
      std::string& response_bytes,
      std::vector<std::string>& response_shared_memory_handles,
      std::vector<std::string>& response_fileshare_handles,
      absl::AnyInvocable<void(grpc::Status) &&> done) = 0;
  virtual grpc::Status IsolateStreamRpcMethodHandler(
      uintptr_t stream_id, const InvokeIsolateRequest& invoke_isolate_request,
      ResponseWriter* invoke_isolate_resp_writer,
      std::shared_ptr<std::vector<std::string>> response_shared_memory_handles,
      std::shared_ptr<std::vector<std::string>> response_fileshare_handles) = 0;
  virtual grpc::Status ForwardStreamingMessage(
      uintptr_t stream_id, const InvokeIsolateRequest& request) = 0;
  virtual grpc::Status RequestStreamClosed(uintptr_t stream_id) = 0;
  virtual grpc::Status OnFileshareEvent(
      std::string_view share_handle,
      enforcer::v1::FileshareEvent::FileshareEventType event_type) = 0;
  virtual std::string_view GetServiceName() = 0;
};

class EzIsolateBridgeImpl : public EzIsolateBridge::CallbackService {
 public:
  explicit EzIsolateBridgeImpl(
      std::shared_ptr<IsolateRpcService> isolate_rpc_service);
  // Used by IsolateRpcServer to add services supported by the Isolate
  // to the service map
  void AddIsolateRpcService(
      std::shared_ptr<IsolateRpcService> isolate_rpc_service);
  // Used to set the Isolate to ready only when all supported services have
  // been added
  void SetIsolateReady();
  grpc::ServerUnaryReactor* InvokeIsolate(
      grpc::CallbackServerContext* context, const InvokeIsolateRequest* request,
      InvokeIsolateResponse* response) override;

  grpc::ServerBidiReactor<InvokeIsolateRequest, InvokeIsolateResponse>*
  StreamInvokeIsolate(grpc::CallbackServerContext* context) override;

  grpc::ServerUnaryReactor* UpdateIsolateState(
      grpc::CallbackServerContext* context,
      const enforcer::v1::UpdateIsolateStateRequest* request,
      enforcer::v1::UpdateIsolateStateResponse* response) override;

 private:
  std::unordered_map<std::string_view, std::shared_ptr<IsolateRpcService>>
      isolate_rpc_service_map_;
  std::atomic<int32_t> current_state_ =
      enforcer::v1::IsolateState::ISOLATE_STATE_UNSPECIFIED;
};

// Isolates must instantiate this, which runs the actual bridge server to
// communicate between the Isolate and the Enforcer.
// Note: Instantiating this blocks the current thread until the server
//       terminates.
class IsolateRpcServer {
 public:
  explicit IsolateRpcServer(
      std::shared_ptr<IsolateRpcService> isolate_rpc_service);
  // Used to add services supported by the Isolate to the bridge
  void AddIsolateRpcService(
      std::shared_ptr<IsolateRpcService> isolate_rpc_service);
  // Used to startup the bridge server once all supported services have
  // been added
  void StartIsolateRpcServer(const std::string& socket_path);
  void StartIsolateRpcServer();
  void ShutdownIsolateRpcServer();

 private:
  std::shared_ptr<EzIsolateBridgeImpl> bridge_;
  std::unique_ptr<grpc::Server> server_;
};

}  // namespace EzIsolateBridgeSdk

#endif  // CORE_CPP_SRC_EZ_ISOLATE_BRIDGE_SERVER_H
