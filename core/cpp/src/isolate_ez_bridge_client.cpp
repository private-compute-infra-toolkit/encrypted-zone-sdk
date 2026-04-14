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

#include "core/cpp/src/isolate_ez_bridge_client.h"

#include <fcntl.h>
#include <grpc/grpc.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/channel_arguments.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/sync_stream.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stack>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/container/flat_hash_map.h"
#include "absl/functional/any_invocable.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"
#include "core/cpp/src/create_fileshare_response.h"
#include "core/cpp/src/mem_share_response.h"
#include "core/cpp/src/utils.h"
#include "enforcer/v1/ez_payload.pb.h"
#include "enforcer/v1/isolate_bridge.pb.h"
#include "enforcer/v1/isolate_ez_bridge.grpc.pb.h"

using enforcer::v1::ControlPlaneMetadata;
using enforcer::v1::CreateMemshareRequest;
using enforcer::v1::CreateMemshareResponse;
using enforcer::v1::DataScopeType;
using enforcer::v1::EventTopic;
using enforcer::v1::FileshareEvent;
using enforcer::v1::InvokeEzRequest;
using enforcer::v1::InvokeEzResponse;
using enforcer::v1::IsolateDataScope;
using enforcer::v1::IsolateEzBridge;
using enforcer::v1::IsolateState;
using enforcer::v1::NotifyIsolateStateRequest;
using enforcer::v1::NotifyIsolateStateResponse;
using enforcer::v1::PublishEventForRequest;
using enforcer::v1::PublishEventForResponse;
using enforcer::v1::StreamSubscribeToRequest;
using enforcer::v1::StreamSubscribeToResponse;

namespace grpc {
inline std::ostream& operator<<(std::ostream& os, const grpc::Status& status) {
  if (status.ok()) {
    return os << "OK";
  }
  // Note: grpc::StatusCode usually HAS an operator<<, so
  // status.error_code() will often print as a string like "NOT_FOUND"
  // or "INTERNAL".
  os << status.error_code() << ": " << status.error_message();

  if (!status.error_details().empty()) {
    os << " (" << status.error_details() << ")";
  }
  return os;
}
}  // namespace grpc

namespace IsolateEzBridgeSdk {

namespace {

bool IsLocalEzInstanceId(absl::string_view ez_instance_id) { return false; }

std::string& GetUdsAddress() {
  static absl::NoDestructor<std::string> path(
      "unix:///enforcer-isolate-shared/isolate-ez-bridge-uds");
  return *path;
}

std::string& GetReadySignalPath() {
  static absl::NoDestructor<std::string> path(
      "/enforcer-isolate-shared/isolate-ez-bridge-uds.ready");
  return *path;
}

constexpr absl::string_view kStagingFileSuffix = ".staging";
}  // namespace

// Soft limit for gRPC metadata size.
constexpr int kGrpcMaxMetadataSize = 32 * 1024 * 1024;  // 32 MB
// Hard limit for gRPC metadata size.
constexpr int kGrpcAbsoluteMaxMetadataSize = 128 * 1024 * 1024;  // 128 MB

class IsolateEzBridgeClientImpl : public IsolateEzBridgeClient {
 public:
  IsolateEzBridgeClientImpl() {
    ipc_message_id_ = 0;
    // TODO: Heavy weight initialization in a constructor is undesirable,
    // consider breaking this out.
    CreateIsolateEzBridgeStub();
  }

  ~IsolateEzBridgeClientImpl() override {
    if (fileshare_event_context_) {
      fileshare_event_context_->TryCancel();
    }
    if (fileshare_event_thread_ && fileshare_event_thread_->joinable()) {
      fileshare_event_thread_->join();
    }
    LOG(INFO) << "IsolateEzBridgeClient destroyed";
  }

  void RegisterLocalHandler(
      const std::string& service_name,
      std::function<grpc::Status(const std::string& method_name,
                                 const std::string& request,
                                 std::string& response)>
          handler) override {
    local_handlers_[service_name] = std::move(handler);
  }

  grpc::Status IsolateRpcCall(grpc::ClientContext* context,
                              const std::string& operator_domain,
                              const std::string& service_name,
                              const std::string& method_name,
                              const std::string& ez_instance_id,
                              const std::string& request_bytes,
                              std::string& response_bytes) override {
    LOG_EVERY_N(INFO, 1000)
        << "IsolateRpcCall EZ outbound call: " << service_name << "."
        << method_name;
    // Create a new context for the outbound call to the bridge, but propagate
    // the deadline from the user's context if available.
    grpc::ClientContext outbound_context;
    if (context != nullptr) {
      outbound_context.set_deadline(context->deadline());
    }

    absl::Notification notification;
    grpc::Status status;

    AsyncIsolateRpcCall(&outbound_context, operator_domain, service_name,
                        method_name, ez_instance_id, request_bytes,
                        &response_bytes,
                        [&notification, &status](grpc::Status s) {
                          status = s;
                          notification.Notify();
                        });

    notification.WaitForNotification();
    return status;
  }

  void AsyncIsolateRpcCall(
      grpc::ClientContext* context, const std::string& operator_domain,
      const std::string& service_name, const std::string& method_name,
      const std::string& ez_instance_id, const std::string& request_bytes,
      std::string* response_bytes,
      absl::AnyInvocable<void(grpc::Status) &&> callback) override {
    LOG_EVERY_N(INFO, 1000)
        << "AsyncIsolateRpcCall EZ outbound call: " << service_name << "."
        << method_name;
    // Check for local handlers first.
    if (IsLocalEzInstanceId(ez_instance_id)) {
      auto it = local_handlers_.find(service_name);
      if (it != local_handlers_.end()) {
        grpc::Status status =
            it->second(method_name, request_bytes, *response_bytes);
        std::move(callback)(status);
        return;
      }
    }

    auto invoke_ez_request = std::make_shared<InvokeEzRequest>();
    uint64_t ipc_message_id = ++ipc_message_id_;

    ControlPlaneMetadata& cp_metadata =
        *invoke_ez_request->mutable_control_plane_metadata();
    cp_metadata.set_ipc_message_id(ipc_message_id);
    cp_metadata.set_responder_is_local(true);
    cp_metadata.set_destination_operator_domain(operator_domain);
    cp_metadata.set_destination_service_name(service_name);
    cp_metadata.set_destination_method_name(method_name);
    cp_metadata.set_destination_ez_instance_id(ez_instance_id);

    IsolateDataScope* isolate_data_scope =
        invoke_ez_request->mutable_isolate_request_iscope()
            ->add_datagram_iscopes();
    isolate_data_scope->set_scope_type(
        DataScopeType::DATA_SCOPE_TYPE_UNSPECIFIED);

    invoke_ez_request->mutable_isolate_request_payload()->add_datagrams(
        request_bytes);

    // Context deadline propagation is handled by the caller (passed in context)
    // We need a response object that lives as long as the call.
    auto async_call_state = std::make_shared<AsyncCallState>(
        AsyncCallState{.client = this,
                       .callback = std::move(callback),
                       .request = std::move(invoke_ez_request),
                       .response = std::make_shared<InvokeEzResponse>(),
                       .response_bytes = response_bytes,
                       .ipc_message_id = ipc_message_id});

    auto* raw_request = async_call_state->request.get();
    auto* raw_response = async_call_state->response.get();

    std::unique_ptr<IsolateEzBridge::Stub>* bridge_stub =
        &isolate_ez_bridge_stub_;
    (*bridge_stub)
        ->async()
        ->InvokeEz(
            context, raw_request, raw_response,
            [this,
             state = std::move(async_call_state)](grpc::Status status) mutable {
              if (!status.ok()) {
                std::move(state->callback)(status);
                return;
              }

              if (state->response->control_plane_metadata().ipc_message_id() !=
                  state->ipc_message_id) {
                std::move(state->callback)(grpc::Status(
                    grpc::StatusCode::INTERNAL, "Mismatched IPC message id"));
                return;
              }

              for (const auto& handle :
                   state->response->control_plane_metadata()
                       .shared_memory_handles()) {
                if (absl::Status s = ProcessReceivedSharedMemoryHandle(handle);
                    !s.ok()) {
                  std::move(state->callback)(
                      grpc::Status(grpc::StatusCode::INTERNAL, s.ToString()));
                  return;
                }
              }

              if (state->response->ez_response_payload().datagrams().empty()) {
                std::move(state->callback)(grpc::Status(
                    grpc::StatusCode::INTERNAL, "Missing response payload"));
                return;
              }

              *state->response_bytes =
                  std::move(*state->response->mutable_ez_response_payload()
                                 ->mutable_datagrams(0));
              std::move(state->callback)(grpc::Status::OK);
            });
  }

  std::unique_ptr<::grpc::ClientReaderWriter<::enforcer::v1::InvokeEzRequest,
                                             ::enforcer::v1::InvokeEzResponse>>
  IsolateStreamRpcCall(grpc::ClientContext* context,
                       const std::string& operator_domain,
                       const std::string& service_name,
                       const std::string& method_name,
                       const std::string& ez_instance_id) override {
    std::unique_ptr<IsolateEzBridge::Stub>* bridge_stub =
        &isolate_ez_bridge_stub_;
    return (*bridge_stub)->StreamInvokeEz(context);
  }

  CreateFileshareResponse CreateFileshare() override {
    grpc::ClientContext context;
    std::unique_ptr<IsolateEzBridge::Stub>* bridge_stub =
        &isolate_ez_bridge_stub_;
    enforcer::v1::CreateFileshareRequest request;
    enforcer::v1::CreateFileshareResponse response;
    grpc::Status status =
        (*bridge_stub)->CreateFileshare(&context, request, &response);
    if (!status.ok()) {
      return {.status = status};
    }
    std::string fileshare_handle =
        std::move(*response.mutable_fileshare_handle());
    std::string shared_path = absl::StrCat("/", fileshare_handle, "/file");
    std::string staging_path =
        absl::StrCat("/", fileshare_handle, "/file", kStagingFileSuffix);
    // Create the shared and staging files in the fileshare directory.
    {
      std::ofstream shared_fs(shared_path);
      std::ofstream staging_fs(staging_path);
    }
    return {
        .status = status,
        .fileshare_handle = std::move(fileshare_handle),
        .shared_path = std::move(shared_path),
        .staging_path = std::move(staging_path),
    };
  }

  absl::Status CommitFileChanges(absl::string_view staging_path) override {
    if (!absl::EndsWith(staging_path, kStagingFileSuffix)) {
      return absl::InvalidArgumentError(
          absl::StrCat("staging_path must end with ", kStagingFileSuffix));
    }
    std::string share_path(staging_path.substr(
        0, staging_path.length() - kStagingFileSuffix.length()));
    if (share_path.empty()) {
      return absl::InvalidArgumentError("staging_path is invalid");
    }
    // Copy the staging file to the shared path. This preserves the staging
    // file.
    std::string tmp_path = absl::StrCat(share_path, ".tmp");
    std::error_code ec;
    std::filesystem::copy(staging_path, tmp_path, ec);
    if (ec.value() != 0) {
      return absl::InternalError(
          absl::StrCat("Failed to copy staging file: ", ec.message()));
    }
    // The rename sys call can be used to atomically replace the file.
    if (rename(tmp_path.c_str(), share_path.c_str()) != 0) {
      return absl::ErrnoToStatus(errno,
                                 "Failed to commit file changes via rename");
    }
    std::string fileshare_handle =
        std::filesystem::path(share_path).parent_path().filename();
    // Notify the Enforcer that the file has been updated.
    grpc::ClientContext context;
    PublishEventForRequest request;
    request.set_topic(EventTopic::EVENT_TOPIC_FILESHARE);
    request.set_handle(std::move(fileshare_handle));
    request.mutable_fileshare_event()->set_event_type(
        FileshareEvent::FILESHARE_EVENT_TYPE_FILE_UPDATED);
    std::unique_ptr<IsolateEzBridge::Stub>* bridge_stub =
        &isolate_ez_bridge_stub_;
    PublishEventForResponse response;
    // Do this synchronously so the caller can easily handle failures.
    return ToAbslStatus(
        (*bridge_stub)->PublishEventFor(&context, request, &response));
  }

  void RegisterFileshareEventHandler(
      absl::AnyInvocable<void(absl::string_view fileshare_handle,
                              FileshareEvent::FileshareEventType event_type)>
          handler) override {
    {
      absl::MutexLock lock(handlers_mutex_);
      fileshare_event_handlers_.push_back(std::move(handler));
    }
    if (!fileshare_stream_started_.exchange(true)) {
      StartFileshareEventStream();
    }
  }

  void ClearFileshareEventHandlers() override {
    absl::MutexLock lock(handlers_mutex_);
    fileshare_event_handlers_.clear();
  }

  // Starts a stream of fileshare events from the Enforcer.
  void StartFileshareEventStream() {
    fileshare_event_context_ = std::make_unique<grpc::ClientContext>();
    if (fileshare_event_thread_ && fileshare_event_thread_->joinable()) {
      fileshare_event_thread_->join();
    }

    auto fileshare_event_handler = [this]() {
      StreamSubscribeToRequest init_req;
      init_req.set_topic(EventTopic::EVENT_TOPIC_FILESHARE);
      std::unique_ptr<IsolateEzBridge::Stub>* bridge_stub =
          &isolate_ez_bridge_stub_;
      std::unique_ptr<::grpc::ClientReader<StreamSubscribeToResponse>> stream =
          (*bridge_stub)
              ->StreamSubscribeTo(fileshare_event_context_.get(), init_req);
      StreamSubscribeToResponse response;
      while (stream->Read(&response)) {
        if (response.topic() != EventTopic::EVENT_TOPIC_FILESHARE ||
            !response.has_fileshare_event()) {
          continue;
        }
        absl::MutexLock lock(handlers_mutex_);
        for (auto& handler : fileshare_event_handlers_) {
          handler(response.handle(), response.fileshare_event().event_type());
        }
      }
      LOG(INFO) << "Fileshare event stream ended";
      fileshare_stream_started_.store(false);
    };
    fileshare_event_thread_ =
        std::make_unique<std::thread>(std::move(fileshare_event_handler));
  }

  bool NewIsolateState(IsolateState new_isolate_state) override {
    return SendNewIsolateState(new_isolate_state);
  }

  MemShareResponse CreateMemShareInternal(int64_t size, char** ptr) override {
    if (ptr == nullptr) {
      return {.status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                     "ptr cannot be null")};
    }

    grpc::ClientContext context;
    std::unique_ptr<IsolateEzBridge::Stub>* bridge_stub =
        &isolate_ez_bridge_stub_;
    auto create_memshare_stream = (*bridge_stub)->CreateMemshare(&context);
    CreateMemshareRequest request;
    request.set_region_size(size);
    if (!create_memshare_stream->Write(request) ||
        !create_memshare_stream->WritesDone()) {
      return {.status = create_memshare_stream->Finish()};
    }
    CreateMemshareResponse response;
    std::string shared_memory_handle;
    if (create_memshare_stream->Read(&response)) {
      shared_memory_handle =
          std::move(*response.mutable_shared_memory_handle());
      std::string filename = "/" + shared_memory_handle;
      int fd = open(filename.c_str(), O_RDWR);
      if (fd < 0) {
        return {.status = grpc::Status(
                    grpc::StatusCode::INTERNAL,
                    absl::ErrnoToStatus(errno, "failed to open mmap file")
                        .ToString())};
      }
      *ptr =
          (char*)mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (*ptr == MAP_FAILED) {
        return {
            .status = grpc::Status(
                grpc::StatusCode::INTERNAL,
                absl::ErrnoToStatus(errno, "mmap failed").ToString()),
        };
      }
    }
    return {
        .status = create_memshare_stream->Finish(),
        .shared_memory_handle = std::move(shared_memory_handle),
    };
  }

  Vec<char> ReceiveMemShareInternal() override {
    if (!received_memshares_.empty()) {
      auto result = received_memshares_.top();
      received_memshares_.pop();
      return result;
    }
    return Vec<char>();
  }

  absl::Status ProcessReceivedSharedMemoryHandle(
      const std::string& shared_memory_handle) {
    const std::string filename = "/" + shared_memory_handle;
    int fd = open(filename.c_str(), O_RDONLY);
    if (fd < 0) {
      return absl::ErrnoToStatus(
          errno, absl::StrCat("Failed to open mmap file: ", filename));
    }
    struct stat statbuf;
    if (fstat(fd, &statbuf) == -1) {
      close(fd);
      return absl::ErrnoToStatus(
          errno, absl::StrCat("fstat failed for mmap file: ", filename));
    }
    char* region_start = static_cast<char*>(
        mmap(nullptr, statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (region_start == MAP_FAILED) {
      return absl::ErrnoToStatus(
          errno,
          absl::StrCat("mmap failed when mapping shared memory: ", filename));
    }

    received_memshares_.push(Vec<char>(region_start, statbuf.st_size));
    return absl::OkStatus();
  }

 protected:
  void CreateIsolateEzBridgeStub() {
    // Only initialize the bridge client after the EZ UDS server is listening.
    // Opening this fifo for read blocks until EZ opens it for write.
    // EZ will unblock this when the UDS server is listening.
    {
      std::ifstream fifo(GetReadySignalPath(), std::ios::in);
      CHECK(fifo.is_open())
          << "Failed to open EZ ready signal fifo. Isolate will be unable to "
             "connect to EZ and not send or receive any requests. Path: "
          << GetReadySignalPath();
    }

    static const std::string DEFAULT_RETRY_POLICY = R"(
      {
        "methodConfig": [
            {
                "name":[{
                    "service": "IsolateEzBridge"
                }],
                "retryPolicy": {
                    "maxAttempts": 5,
                    "initialBackoff": "0.3s",
                    "maxBackoff": "1s",
                    "backoffMultiplier": 1,
                    "retryableStatusCodes": [
                      "UNAVAILABLE"
                    ]
                }
            }
        ]
      }
    )";
    grpc::ChannelArguments channel_args;
    // Set Set soft limit to 32 MB
    channel_args.SetInt(GRPC_ARG_MAX_METADATA_SIZE, kGrpcMaxMetadataSize);
    // Set hard limit to 128 MB
    channel_args.SetInt(GRPC_ARG_ABSOLUTE_MAX_METADATA_SIZE,
                        kGrpcAbsoluteMaxMetadataSize);

    channel_args.SetServiceConfigJSON(DEFAULT_RETRY_POLICY);
    // Tonic server requires some authority when using UDS.
    // See: https://github.com/hyperium/h2/pull/487#issuecomment-999633424
    channel_args.SetSslTargetNameOverride("not_used");
    isolate_ez_bridge_stub_ =
        IsolateEzBridge::NewStub(grpc::CreateCustomChannel(
            GetUdsAddress(), grpc::InsecureChannelCredentials(), channel_args));
  }

  bool SendNewIsolateState(IsolateState new_isolate_state) {
    grpc::ClientContext context;
    std::unique_ptr<IsolateEzBridge::Stub>* bridge_stub =
        &isolate_ez_bridge_stub_;
    auto notify_state_stream = (*bridge_stub)->NotifyIsolateState(&context);
    NotifyIsolateStateRequest request;
    request.set_new_isolate_state(new_isolate_state);
    if (!notify_state_stream->Write(request) ||
        !notify_state_stream->WritesDone()) {
      LOG(ERROR) << "Failure to send new IsolateState";
      return false;
    }
    // Read the response, even though it contains nothing right now
    NotifyIsolateStateResponse response;
    bool success = notify_state_stream->Read(&response);
    auto status = notify_state_stream->Finish();
    if (!success) {
      LOG(WARNING)
          << "Failed to read IsolateState response, stream finished with "
          << "status: " << status;
    }
    return status.ok();
  }

 private:
  struct AsyncCallState {
    IsolateEzBridgeClientImpl* client;
    absl::AnyInvocable<void(grpc::Status) &&> callback;
    std::shared_ptr<InvokeEzRequest> request;
    std::shared_ptr<InvokeEzResponse> response;
    std::string* response_bytes;
    uint64_t ipc_message_id;
  };
  std::unique_ptr<IsolateEzBridge::Stub> isolate_ez_bridge_stub_;
  std::atomic_uint64_t ipc_message_id_;
  std::stack<Vec<char>> received_memshares_;
  absl::flat_hash_map<
      std::string, std::function<grpc::Status(
                       const std::string&, const std::string&, std::string&)>>
      local_handlers_;
  std::vector<absl::AnyInvocable<void(absl::string_view,
                                      FileshareEvent::FileshareEventType)>>
      fileshare_event_handlers_;
  absl::Mutex handlers_mutex_;
  std::atomic<bool> fileshare_stream_started_{false};
  std::unique_ptr<grpc::ClientContext> fileshare_event_context_;
  std::unique_ptr<std::thread> fileshare_event_thread_;
};

// This is a mix of pImpl-style hidden implementation, and a static factory
// pattern. Currently only returns a global singleton instance, initialized
// lazily when this method is first invoked. (Note: C++11 seems to guarantee
// thread-safety for this singleton pattern.)
IsolateEzBridgeClient* g_instance_for_testing = nullptr;

void IsolateEzBridgeClient::SetInstanceForTesting(
    IsolateEzBridgeClient* instance) {
  g_instance_for_testing = instance;
}

IsolateEzBridgeClient& IsolateEzBridgeClient::GetInstance() {
  if (g_instance_for_testing) {
    return *g_instance_for_testing;
  }
  // TODO: Consider switching to thread-local.
  static IsolateEzBridgeClientImpl instance;
  return instance;
}

absl::Status IsolateEzBridgeClient::ProcessReceivedSharedMemoryHandle(
    const std::string& shared_memory_handle) {
  return static_cast<IsolateEzBridgeClientImpl*>(this)
      ->ProcessReceivedSharedMemoryHandle(shared_memory_handle);
}

void IsolateEzBridgeClient::SetUdsAddress(std::string path) {
  GetUdsAddress() = std::move(path);
}

void IsolateEzBridgeClient::SetReadySignalPath(std::string path) {
  GetReadySignalPath() = std::move(path);
}

}  // namespace IsolateEzBridgeSdk
