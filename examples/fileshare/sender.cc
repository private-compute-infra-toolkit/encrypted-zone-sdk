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

#include <unistd.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "absl/status/status.h"
#include "core/cpp/src/ez_isolate_bridge_server.h"
#include "core/cpp/src/isolate_ez_bridge_client.h"
#include "examples/fileshare/proto/fileshare.pb.h"

using IsolateEzBridgeSdk::IsolateEzBridgeClient;

class FileProviderImpl final : public EzIsolateBridgeSdk::IsolateRpcService {
 public:
  void IsolateRpcMethodHandler(
      grpc::CallbackServerContext* context, const std::string& method_name,
      const ::google::protobuf::RepeatedPtrField<std::string>& request_bytes,
      std::string& response_bytes,
      std::vector<std::string>& response_shared_memory_handles,
      std::vector<std::string>& response_fileshare_handles,
      absl::AnyInvocable<void(grpc::Status) &&> done) override {
    grpc::Status status;
    if (method_name == "GetFile") {
      status = HandleGetFile(response_bytes, response_fileshare_handles);
    } else if (method_name == "TriggerUpdates") {
      status = HandleTriggerUpdates();
    } else {
      status = grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Unknown method");
    }
    std::move(done)(status);
  }

 private:
  grpc::Status HandleGetFile(
      std::string& response_bytes,
      std::vector<std::string>& response_fileshare_handles) {
    auto& bridge_client = IsolateEzBridgeClient::GetInstance();
    std::cout << "Creating fileshare..." << std::endl;
    auto create_response = bridge_client.CreateFileshare();
    if (!create_response.status.ok()) {
      std::cerr << "Failed to create fileshare: "
                << create_response.status.error_message() << std::endl;
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "Failed to create fileshare");
    }
    last_handle_ = create_response.fileshare_handle;
    last_staging_path_ = create_response.staging_path;
    std::cout << "Sender got handle: " << last_handle_ << std::endl;
    std::cout << "Writing to staging file: " << last_staging_path_ << std::endl;
    {
      std::ofstream os(last_staging_path_, std::ios::binary);
      os << "Hello from sender via fileshare!";
    }
    if (absl::Status s = bridge_client.CommitFileChanges(last_staging_path_);
        !s.ok()) {
      std::cerr << "Failed to commit changes: " << s.message() << std::endl;
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "Failed to commit initial file changes");
    }
    fileshare::GetFileResponse response;
    response.set_fileshare_handle(last_handle_);
    response_bytes = response.SerializeAsString();
    response_fileshare_handles.push_back(last_handle_);
    return grpc::Status::OK;
  }

  grpc::Status HandleTriggerUpdates() {
    auto& bridge_client = IsolateEzBridgeClient::GetInstance();
    {
      std::cout << "Writing second update to staging file..." << std::endl;
      std::ofstream os(last_staging_path_, std::ios::binary);
      os << "Second update from sender!";
    }
    if (absl::Status s = bridge_client.CommitFileChanges(last_staging_path_);
        !s.ok()) {
      std::cerr << "Failed to commit changes: " << s.message() << std::endl;
    }
    return grpc::Status::OK;
  }

 public:
  grpc::Status IsolateStreamRpcMethodHandler(
      uintptr_t stream_id,
      const enforcer::v1::InvokeIsolateRequest& invoke_isolate_request,
      EzIsolateBridgeSdk::ResponseWriter* invoke_isolate_resp_writer,
      std::shared_ptr<std::vector<std::string>> response_shared_memory_handles,
      std::shared_ptr<std::vector<std::string>> response_fileshare_handles)
      override {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Not supported");
  }

  grpc::Status ForwardStreamingMessage(
      uintptr_t stream_id,
      const enforcer::v1::InvokeIsolateRequest& request) override {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Not supported");
  }

  grpc::Status RequestStreamClosed(uintptr_t stream_id) override {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Not supported");
  }

  grpc::Status OnFileshareEvent(
      std::string_view share_handle,
      enforcer::v1::FileshareEvent::FileshareEventType event_type) override {
    return grpc::Status::OK;
  }

  std::string_view GetServiceName() override { return "FileProvider"; }

 private:
  std::string last_handle_;
  std::string last_staging_path_;
};

int main(int argc, char** argv) {
  std::cout << "Sender started." << std::endl;
  auto& bridge_client = IsolateEzBridgeClient::GetInstance();
  bridge_client.NewIsolateState(
      enforcer::v1::IsolateState::ISOLATE_STATE_READY);
  auto service = std::make_shared<FileProviderImpl>();
  EzIsolateBridgeSdk::IsolateRpcServer server(service);
  if (argc > 1 && argv[1][0] != '\0') {
    server.StartIsolateRpcServer(argv[1]);
  } else {
    server.StartIsolateRpcServer();
  }
  return 0;
}
