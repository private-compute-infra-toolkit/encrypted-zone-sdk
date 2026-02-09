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

#include <gtest/gtest.h>

#include <future>
#include <memory>
#include <string>

#include "absl/synchronization/notification.h"
#include "absl/time/time.h"
#include "core/cpp/src/isolate_ez_bridge_client.h"
#include "enforcer/v1/isolate_ez_bridge.grpc.pb.h"
#include "examples/helloworld/proto/greeter.grpc.pb.h"
#include "grpcpp/grpcpp.h"
#include "grpcpp/server_builder.h"

using grpc::ClientContext;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

#include <filesystem>
#include <fstream>

static constexpr absl::Duration kTestTimeout = absl::Seconds(5);

class FakeIsolateEzBridgeService final
    : public enforcer::v1::IsolateEzBridge::Service {
 public:
  grpc::Status InvokeEz(grpc::ServerContext* context,
                        const enforcer::v1::InvokeEzRequest* request,
                        enforcer::v1::InvokeEzResponse* response) override {
    auto* metadata = response->mutable_control_plane_metadata();
    metadata->set_ipc_message_id(
        request->control_plane_metadata().ipc_message_id());

    const auto& service_name =
        request->control_plane_metadata().destination_service_name();
    const auto& method_name =
        request->control_plane_metadata().destination_method_name();

    if (service_name == "Greeter" && method_name == "SayHello") {
      if (request->isolate_request_payload().datagrams_size() > 0) {
        helloworld::HelloRequest hello_request;
        if (hello_request.ParseFromString(
                request->isolate_request_payload().datagrams(0))) {
          helloworld::HelloReply hello_reply;
          hello_reply.set_message("Hello " + hello_request.name());

          auto* payload = response->mutable_ez_response_payload();
          payload->add_datagrams(hello_reply.SerializeAsString());
          return grpc::Status::OK;
        }
      }
    }
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "Method not implemented");
  }

  grpc::Status StreamInvokeEz(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<enforcer::v1::InvokeEzResponse,
                               enforcer::v1::InvokeEzRequest>* stream)
      override {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        "StreamInvokeEz not implemented");
  }
};

class HelloworldAsyncTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create temporary directory for sockets
    temp_dir_ =
        std::filesystem::temp_directory_path() / "helloworld_async_test";
    std::filesystem::create_directories(temp_dir_);

    uds_path_ = "unix://" + (temp_dir_ / "socket").string();
    ready_file_ = temp_dir_ / "ready";

    // Override paths
    IsolateEzBridgeSdk::IsolateEzBridgeClient::SetUdsAddress(uds_path_);
    IsolateEzBridgeSdk::IsolateEzBridgeClient::SetReadySignalPath(
        ready_file_.string());

    // Create ready file to unblock client
    std::ofstream ofs(ready_file_);
    ofs << "ready";
    ofs.close();

    // Start fake server
    grpc::ServerBuilder builder;
    builder.AddListeningPort(uds_path_, grpc::InsecureServerCredentials());
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
  }

  void TearDown() override {
    if (server_) {
      server_->Shutdown();
    }
    std::filesystem::remove_all(temp_dir_);
  }

  std::filesystem::path temp_dir_;
  std::string uds_path_;
  std::filesystem::path ready_file_;
  FakeIsolateEzBridgeService service_;
  std::unique_ptr<grpc::Server> server_;
};

TEST_F(HelloworldAsyncTest, DISABLED_AsyncSayHelloWorks) {
  ClientContext context;
  HelloRequest request;
  request.set_name("AsyncWorld");
  HelloReply response;
  absl::Notification notification;
  std::unique_ptr<Greeter::Stub> stub = Greeter::NewStub("test-domain");
  stub->async()->SayHello(
      &context, &request, &response, [&notification](Status status) {
        EXPECT_TRUE(status.ok()) << "RPC failed: " << status.error_message();
        notification.Notify();
      });

  // Wait for the callback
  ASSERT_TRUE(notification.WaitForNotificationWithTimeout(kTestTimeout));
  EXPECT_EQ(response.message(), "Hello AsyncWorld");
}

TEST_F(HelloworldAsyncTest, DISABLED_SyncSayHelloWorks) {
  auto stub = Greeter::NewStub("test-domain");

  ClientContext context;
  HelloRequest request;
  request.set_name("SyncWorld");
  HelloReply response;

  Status status = stub->SayHello(&context, request, &response);
  EXPECT_TRUE(status.ok()) << "RPC failed: " << status.error_message();
  EXPECT_EQ(response.message(), "Hello SyncWorld");
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
