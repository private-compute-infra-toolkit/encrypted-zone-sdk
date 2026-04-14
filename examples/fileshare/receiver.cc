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
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "absl/strings/str_cat.h"
#include "core/cpp/src/ez_isolate_bridge_server.h"
#include "core/cpp/src/isolate_ez_bridge_client.h"
#include "examples/fileshare/proto/fileshare.grpc.pb.h"
#include "examples/fileshare/proto/fileshare.pb.h"

namespace {

// Local base class to provide default implementations for IsolateRpcService.
class BaseService : public EzIsolateBridgeSdk::IsolateRpcService {
 public:
  void IsolateRpcMethodHandler(
      grpc::CallbackServerContext*, const std::string&,
      const google::protobuf::RepeatedPtrField<std::string>&, std::string&,
      std::vector<std::string>&, std::vector<std::string>&,
      absl::AnyInvocable<void(grpc::Status) &&> done) override {
    std::move(done)(
        grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Not supported"));
  }

  grpc::Status IsolateStreamRpcMethodHandler(
      uintptr_t, const enforcer::v1::InvokeIsolateRequest&,
      EzIsolateBridgeSdk::ResponseWriter*,
      std::shared_ptr<std::vector<std::string>>,
      std::shared_ptr<std::vector<std::string>>) override {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Not supported");
  }

  grpc::Status ForwardStreamingMessage(
      uintptr_t, const enforcer::v1::InvokeIsolateRequest&) override {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Not supported");
  }

  grpc::Status RequestStreamClosed(uintptr_t) override {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Not supported");
  }

  grpc::Status OnFileshareEvent(
      std::string_view,
      enforcer::v1::FileshareEvent::FileshareEventType) override {
    return grpc::Status::OK;
  }
};

class FileReceiverImpl final : public BaseService {
 public:
  grpc::Status OnFileshareEvent(
      std::string_view share_handle,
      enforcer::v1::FileshareEvent::FileshareEventType event_type) override {
    if (share_handle == last_handle_ &&
        event_type ==
            enforcer::v1::FileshareEvent::FILESHARE_EVENT_TYPE_FILE_UPDATED) {
      std::cout << "Receiver got update notification for: " << share_handle
                << std::endl;

      std::string share_path = absl::StrCat("/", share_handle, "/file");
      std::ifstream ifs(share_path);
      if (ifs.is_open()) {
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            (std::istreambuf_iterator<char>()));
        std::cout << "Receiver read: " << content << std::endl;
      }
      exit(0);
    }
    return grpc::Status::OK;
  }

  std::string_view GetServiceName() override { return "FileReceiver"; }

  void SetLastHandle(const std::string& handle) { last_handle_ = handle; }

 private:
  std::string last_handle_;
};

void RunClientLogic(std::shared_ptr<FileReceiverImpl> service) {
  sleep(2);  // Wait for sender to start
  std::cout << "Receiver client logic started." << std::endl;
  std::unique_ptr<fileshare::FileProvider::Stub> sender_client =
      fileshare::FileProvider::NewStub("fileshare_test");
  fileshare::GetFileRequest req;
  fileshare::GetFileResponse resp;
  grpc::ClientContext context;
  if (auto status = sender_client->GetFile(&context, req, &resp);
      !status.ok()) {
    std::cerr << "GetFile RPC failed: " << status.error_message() << std::endl;
    return;
  }
  std::cout << "Receiver got handle: " << resp.fileshare_handle() << std::endl;
  service->SetLastHandle(resp.fileshare_handle());
  fileshare::TriggerUpdatesRequest trigger_req;
  fileshare::TriggerUpdatesResponse trigger_resp;
  grpc::ClientContext trigger_context;
  if (auto status = sender_client->TriggerUpdates(&trigger_context, trigger_req,
                                                  &trigger_resp);
      !status.ok()) {
    std::cerr << "TriggerUpdates RPC failed: " << status.error_message()
              << std::endl;
    return;
  }
  std::cout << "Receiver finished client logic." << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << "Receiver started." << std::endl;
  auto service = std::make_shared<FileReceiverImpl>();
  std::thread([service] { RunClientLogic(service); }).detach();
  EzIsolateBridgeSdk::IsolateRpcServer server(service);
  if (argc > 1 && argv[1][0] != '\0') {
    server.StartIsolateRpcServer(argv[1]);
  } else {
    server.StartIsolateRpcServer();
  }
  return 0;
}
