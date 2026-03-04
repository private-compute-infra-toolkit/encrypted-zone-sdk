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

#include "core/cpp/src/ez_isolate_bridge_server.h"

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/server_callback.h>
#include <grpcpp/support/status.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/flags/flag.h"
#include "absl/functional/any_invocable.h"
#include "absl/log/log.h"
#include "absl/numeric/int128.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"
#include "core/cpp/src/isolate_ez_bridge_client.h"
#include "enforcer/v1/ez_isolate_bridge.grpc.pb.h"
#include "enforcer/v1/ez_payload.pb.h"
#include "enforcer/v1/isolate_bridge.pb.h"
#include "google/protobuf/message_lite.h"

ABSL_FLAG(absl::Duration, ez_isolate_bridge_server_local_handler_timeout,
          absl::Minutes(2), "Default timeout for local handler RPC calls.");

namespace {

using enforcer::v1::ControlPlaneMetadata;
using enforcer::v1::DataScopeType;
using enforcer::v1::EzIsolateBridge;
using enforcer::v1::EzPayloadData;
using enforcer::v1::EzPayloadIsolateScope;
using enforcer::v1::InvokeIsolateRequest;
using enforcer::v1::InvokeIsolateResponse;
using enforcer::v1::IsolateDataScope;
using enforcer::v1::IsolateState;
using enforcer::v1::IsolateStatus;
using IsolateRpcServiceSharedPtr =
    std::shared_ptr<EzIsolateBridgeSdk::IsolateRpcService>;

constexpr std::string_view kDefaultSocketPath =
    "/enforcer-isolate-shared/ez-isolate-bridge-uds";
constexpr std::string_view kMaxDecodingMessageSizeEnvVar =
    "EZ_MAX_DECODING_MESSAGE_SIZE";

}  // namespace

namespace EzIsolateBridgeSdk {

// Used to populate InvokeIsolateResponse with INVALID_ARGUMENT code and custom
// message
void CreateInvalidArgumentResponse(const std::string& message,
                                   uint64_t ipc_msg_id,
                                   InvokeIsolateResponse& response) {
  IsolateStatus* isolate_status = response.mutable_status();
  isolate_status->set_message(message);
  isolate_status->set_code(grpc::StatusCode::INVALID_ARGUMENT);
  LOG(INFO) << "InvalidArgumentResponse [ipc_msg_id: " << ipc_msg_id
            << "]: " << message;
  response.mutable_control_plane_metadata()->set_ipc_message_id(ipc_msg_id);
}

// Used to forward request to Isolate; Invokes unary IsolateRpcMethodHandler
void ForwardRequest(grpc::CallbackServerContext* context,
                    std::shared_ptr<IsolateRpcService> isolate_rpc_service,
                    const InvokeIsolateRequest& request,
                    InvokeIsolateResponse& response,
                    absl::AnyInvocable<void(grpc::Status) &&> done) {
  const ControlPlaneMetadata& request_cp_metadata =
      request.control_plane_metadata();
  const uint64_t ipc_msg_id = request_cp_metadata.ipc_message_id();
  LOG(INFO) << "ipc_msg_id: " << ipc_msg_id;

  if (const std::string& service_name =
          request_cp_metadata.destination_service_name();
      service_name != isolate_rpc_service->GetServiceName()) {
    CreateInvalidArgumentResponse(
        absl::StrCat("Mismatched service name: expected ",
                     isolate_rpc_service->GetServiceName(), ", but got ",
                     service_name),
        ipc_msg_id, response);
    std::move(done)(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "Mismatched service name"));
    return;
  }

  const std::string& method_name =
      request_cp_metadata.destination_method_name();
  if (request.isolate_input().datagrams().empty()) {
    CreateInvalidArgumentResponse("Missing datagram", ipc_msg_id, response);
    std::move(done)(
        grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Missing datagram"));
    return;
  }

  const auto& datagrams = request.isolate_input().datagrams();
  struct ResponseState {
    std::string bytes;
    std::vector<std::string> shared_memory_handles;
  };
  auto response_state = std::make_unique<ResponseState>();
  auto* raw_state = response_state.get();

  // Forwarding happens here
  VLOG(5) << "Forwarding request to IsolateRpcMethodHandler";
  isolate_rpc_service->IsolateRpcMethodHandler(
      context, method_name, datagrams, raw_state->bytes,
      raw_state->shared_memory_handles,
      [&response, response_state = std::move(response_state), ipc_msg_id,
       done = std::move(done)](grpc::Status status) mutable {
        VLOG(3) << "IsolateRpcMethodHandler completed with status: " << status;
        ControlPlaneMetadata* response_cp_metadata =
            response.mutable_control_plane_metadata();
        response_cp_metadata->set_ipc_message_id(ipc_msg_id);
        response_cp_metadata->set_responder_is_local(true);
        // TODO: Validate the memory shares before returning them
        for (const std::string& handle :
             response_state->shared_memory_handles) {
          response_cp_metadata->add_shared_memory_handles(handle);
        }

        if (!status.ok()) {
          std::move(done)(status);
          return;
        } else {
          EzPayloadIsolateScope* ez_payload_iscope =
              response.mutable_isolate_output_iscope();
          EzPayloadData* ez_payload_data = response.mutable_isolate_output();
          IsolateDataScope* isolate_data_scope =
              ez_payload_iscope->add_datagram_iscopes();
          isolate_data_scope->set_scope_type(
              DataScopeType::DATA_SCOPE_TYPE_UNSPECIFIED);
          ez_payload_data->add_datagrams(response_state->bytes);
        }

        VLOG(5) << "Invoking done callback, setting "
                << "invoke_isolate_response: " << response.DebugString();
        std::move(done)(status);
      });
}

}  // namespace EzIsolateBridgeSdk

namespace {

class Reactor : public grpc::ServerBidiReactor<InvokeIsolateRequest,
                                               InvokeIsolateResponse>,
                public EzIsolateBridgeSdk::ResponseWriter {
 public:
  explicit Reactor(
      std::unordered_map<std::string_view, IsolateRpcServiceSharedPtr>
          isolate_rpc_service_map,
      std::atomic<int32_t>* current_state)
      : isolate_rpc_service_map_(std::move(isolate_rpc_service_map)),
        current_state_(current_state) {
    StartRead(&invoke_isolate_req_);
  }

  void WriteResponse(
      std::unique_ptr<InvokeIsolateResponse> response_ptr) override {
    writing_in_progress_.wait(true);
    writing_in_progress_.store(true);
    // Store the response to prevent it from going out of scope while the
    // async write is still pending.
    StartWrite(response_ptr.get());
    // Keep the response alive until OnWriteDone is called.
    absl::MutexLock lock(&pending_writes_mutex_);
    pending_writes_.push_back(std::move(response_ptr));
  }

  // This ends the stream with the enforcer.
  // Only call when you have received all responses from real server.
  void FinishStream(const grpc::Status& status) override { Finish(status); }

  void OnReadDone(bool ok) override {
    IsolateRpcServiceSharedPtr isolate_rpc_service = GetService();
    if (isolate_rpc_service == nullptr) {
      const std::string error_message =
          absl::StrCat("Service not supported by isolate: ",
                       invoke_isolate_req_.control_plane_metadata()
                           .destination_service_name());
      LOG(ERROR) << error_message;
      Finish(grpc::Status(grpc::StatusCode::UNKNOWN, error_message));
      return;
    }
    if (!ok) {
      // The client is done sending messages, so the service needs to signal to
      // let the end user's `stream->Read()` return false.
      auto status = isolate_rpc_service->RequestStreamClosed(stream_id_);
      return;
    }

    grpc::Status status;
    if (!is_rpc_started_) {
      int32_t current_state = current_state_->load(std::memory_order_acquire);
      if (current_state != enforcer::v1::IsolateState::ISOLATE_STATE_READY) {
        std::string error_message = absl::StrCat(
            "Isolate is not ready. Current state: ", current_state);
        Finish(
            grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, error_message));
        return;
      }
      is_rpc_started_ = true;
      std::shared_ptr<std::vector<std::string>> response_shared_memory_handles =
          std::make_shared<std::vector<std::string>>();
      status = isolate_rpc_service->IsolateStreamRpcMethodHandler(
          stream_id_, invoke_isolate_req_, this,
          response_shared_memory_handles);
    } else {
      status = isolate_rpc_service->ForwardStreamingMessage(
          stream_id_, invoke_isolate_req_);
    }
    if (!status.ok()) {
      LOG(ERROR) << status.error_message();
    }

    StartRead(&invoke_isolate_req_);
  }

  void OnWriteDone(bool ok) override {
    writing_in_progress_.store(false);
    writing_in_progress_.notify_one();
    // Free memory associated with the write that just completed.
    if (absl::MutexLock lock(&pending_writes_mutex_);
        !pending_writes_.empty()) {
      pending_writes_.pop_front();
    }

    if (!ok) {
      Finish(grpc::Status(grpc::StatusCode::UNKNOWN,
                          "Unexpected failure while writing"));
      return;
    }
  }

  // This will get called after we call Finish() on the reactor.
  void OnDone() override {
    LOG(INFO) << "RPC Completed";
    if (IsolateRpcServiceSharedPtr isolate_rpc_service = GetService();
        isolate_rpc_service == nullptr) {
      LOG(ERROR) << "Service not supported by isolate: "
                 << invoke_isolate_req_.control_plane_metadata()
                        .destination_service_name();
      return;
    }
    delete this;
  }

  void OnCancel() override { LOG(INFO) << "RPC Cancelled"; }

 private:
  IsolateRpcServiceSharedPtr GetService() {
    ControlPlaneMetadata control_plane_metadata =
        invoke_isolate_req_.control_plane_metadata();
    std::string_view service_name =
        control_plane_metadata.destination_service_name();
    auto service_it = isolate_rpc_service_map_.find(service_name);
    if (service_it == isolate_rpc_service_map_.end()) {
      return nullptr;
    }
    return service_it->second;
  }

  std::unordered_map<std::string_view, IsolateRpcServiceSharedPtr>
      isolate_rpc_service_map_;
  // Keep response objects alive until they are written.
  std::deque<std::unique_ptr<InvokeIsolateResponse>> pending_writes_
      ABSL_GUARDED_BY(pending_writes_mutex_);
  absl::Mutex pending_writes_mutex_;
  InvokeIsolateRequest invoke_isolate_req_;
  bool is_rpc_started_ = false;
  const uintptr_t stream_id_ = reinterpret_cast<uintptr_t>(this);
  std::atomic<bool> writing_in_progress_{false};
  std::atomic<int32_t>* current_state_;
};
}  // namespace

namespace EzIsolateBridgeSdk {

EzIsolateBridgeImpl::EzIsolateBridgeImpl(
    std::shared_ptr<IsolateRpcService> isolate_rpc_service) {
  AddIsolateRpcService(std::move(isolate_rpc_service));
}

grpc::ServerUnaryReactor* EzIsolateBridgeImpl::InvokeIsolate(
    grpc::CallbackServerContext* context, const InvokeIsolateRequest* request,
    InvokeIsolateResponse* response) {
  const ControlPlaneMetadata& request_cp_metadata =
      request->control_plane_metadata();
  int32_t current_state = current_state_.load(std::memory_order_acquire);
  if (current_state != enforcer::v1::IsolateState::ISOLATE_STATE_READY) {
    std::string error_message =
        absl::StrCat("Isolate is not ready. Current state: ", current_state);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(
        grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, error_message));
    return reactor;
  }
  const std::string_view service_name =
      request_cp_metadata.destination_service_name();
  if (auto service_it = isolate_rpc_service_map_.find(service_name);
      service_it == isolate_rpc_service_map_.end()) {
    const std::string error_message =
        absl::StrCat("Service not supported by isolate: ", service_name);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNKNOWN, error_message));
    return reactor;
  } else {
    auto service_handler = service_it->second;
    auto* reactor = context->DefaultReactor();
    // TODO: Add custom impl once we support cancellations
    ForwardRequest(context, service_handler, *request, *response,
                   [reactor, response](grpc::Status status) {
                     VLOG(4)
                         << "Invoking reactor finish with status: " << status
                         << " and invoke isolate completed: "
                         << response->DebugString();
                     reactor->Finish(status);
                   });
    return reactor;
  }
}

grpc::ServerBidiReactor<InvokeIsolateRequest, InvokeIsolateResponse>*
EzIsolateBridgeImpl::StreamInvokeIsolate(grpc::CallbackServerContext* context) {
  return new Reactor(isolate_rpc_service_map_, &current_state_);
}

grpc::ServerUnaryReactor* EzIsolateBridgeImpl::UpdateIsolateState(
    grpc::CallbackServerContext* context,
    const enforcer::v1::UpdateIsolateStateRequest* request,
    enforcer::v1::UpdateIsolateStateResponse* response) {
  enforcer::v1::IsolateState move_to_state = request->move_to_state();
  LOG(INFO) << "Received update isolate state request: " << move_to_state;
  enforcer::v1::IsolateState new_state = move_to_state;
  if (move_to_state == enforcer::v1::IsolateState::ISOLATE_STATE_STARTING) {
    new_state = enforcer::v1::IsolateState::ISOLATE_STATE_READY;
  }
  current_state_.store(new_state, std::memory_order_release);
  response->set_current_state(new_state);
  auto* reactor = context->DefaultReactor();
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

void EzIsolateBridgeImpl::AddIsolateRpcService(
    std::shared_ptr<IsolateRpcService> isolate_rpc_service) {
  IsolateEzBridgeSdk::IsolateEzBridgeClient::GetInstance().RegisterLocalHandler(
      std::string(isolate_rpc_service->GetServiceName()),
      [isolate_rpc_service](const std::string& service_name,
                            const std::string& request, std::string& response) {
        std::vector<std::string> response_shared_memory_handles;
        google::protobuf::RepeatedPtrField<std::string> request_bytes;
        *request_bytes.Add() = request;
        absl::Notification notification;
        grpc::Status status_result;
        isolate_rpc_service->IsolateRpcMethodHandler(
            /*context=*/nullptr, service_name, request_bytes, response,
            response_shared_memory_handles,
            [&status_result, &notification](grpc::Status status) {
              status_result = status;
              notification.Notify();
            });
        if (!notification.WaitForNotificationWithTimeout(absl::GetFlag(
                FLAGS_ez_isolate_bridge_server_local_handler_timeout))) {
          status_result = grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                                       "Deadline exceeded");
        }
        return status_result;
      });
  isolate_rpc_service_map_.insert(
      {isolate_rpc_service->GetServiceName(), std::move(isolate_rpc_service)});
}

void EzIsolateBridgeImpl::SetIsolateReady() {
  current_state_.store(enforcer::v1::IsolateState::ISOLATE_STATE_READY,
                       std::memory_order_release);
  IsolateEzBridgeSdk::IsolateEzBridgeClient::GetInstance().NewIsolateState(
      IsolateState::ISOLATE_STATE_READY);
}

IsolateRpcServer::IsolateRpcServer(
    std::shared_ptr<IsolateRpcService> isolate_rpc_service)
    : bridge_(std::make_shared<EzIsolateBridgeImpl>(
          std::move(isolate_rpc_service))) {}

void IsolateRpcServer::AddIsolateRpcService(
    std::shared_ptr<IsolateRpcService> isolate_rpc_service) {
  bridge_->AddIsolateRpcService(std::move(isolate_rpc_service));
}

void IsolateRpcServer::StartIsolateRpcServer() {
  StartIsolateRpcServer(std::string(kDefaultSocketPath));
}

void IsolateRpcServer::StartIsolateRpcServer(const std::string& socket_path) {
  LOG(INFO) << "Starting EzIsolateBridge Server. Socket path: " << socket_path;
  grpc::ServerBuilder builder;
  if (const char* env_var_value =
          std::getenv(kMaxDecodingMessageSizeEnvVar.data());
      env_var_value != nullptr) {
    if (int max_decoding_message_size;
        !absl::SimpleAtoi(env_var_value, &max_decoding_message_size)) {
      LOG(ERROR) << "Invalid value for " << kMaxDecodingMessageSizeEnvVar
                 << ". Using default max message size.";
    } else {
      builder.SetMaxReceiveMessageSize(max_decoding_message_size);
    }
  }
  builder.AddListeningPort("unix:" + socket_path,
                           grpc::InsecureServerCredentials());
  builder.RegisterService(&(*bridge_));
  server_ = builder.BuildAndStart();
  LOG(INFO) << "Server listening on " << socket_path;
  bridge_->SetIsolateReady();
  server_->Wait();
}

void IsolateRpcServer::ShutdownIsolateRpcServer() {
  if (server_ != nullptr) {
    server_->Shutdown();
  }
}

}  // namespace EzIsolateBridgeSdk
