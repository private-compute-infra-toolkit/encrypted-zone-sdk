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

#include "core/cpp/src/isolate_ez_bridge_client.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include <grpcpp/grpcpp.h>

#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "core/cpp/test/mock_isolate_ez_bridge_client.h"
#include "enforcer/v1/ez_payload.pb.h"
#include "enforcer/v1/isolate_ez_bridge.grpc.pb.h"

namespace IsolateEzBridgeSdk {
namespace {

using ::enforcer::v1::InvokeEzRequest;
using ::enforcer::v1::InvokeEzResponse;
using ::IsolateEzBridgeSdk::IsolateEzBridgeClient;
using ::IsolateEzBridgeSdk::MockBridgeClient;

class MockIsolateEzBridgeService
    : public ::enforcer::v1::IsolateEzBridge::Service {
 public:
  grpc::Status InvokeEz(grpc::ServerContext* context,
                        const InvokeEzRequest* request,
                        InvokeEzResponse* response) override {
    // Check if deadline is set
    if (absl::FromChrono(context->deadline()) == absl::InfiniteFuture()) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "Deadline not set");
    }

    // Check if deadline is in the future (sanity check)
    if (absl::FromChrono(context->deadline()) < absl::Now()) {
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                          "Deadline already expired");
    }

    // Return a dummy response
    response->mutable_control_plane_metadata()->set_ipc_message_id(
        request->control_plane_metadata().ipc_message_id());
    response->mutable_ez_response_payload()->add_datagrams("response");
    return grpc::Status::OK;
  }
};

class IsolateEzBridgeClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create temporary directory for sockets
    temp_dir_ =
        std::filesystem::temp_directory_path() / "isolate_ez_bridge_test";
    std::filesystem::create_directories(temp_dir_);

    uds_path_ = "unix://" + (temp_dir_ / "socket").string();
    ready_file_ = temp_dir_ / "ready";

    // Override paths
    IsolateEzBridgeClient::SetUdsAddress(uds_path_);
    IsolateEzBridgeClient::SetReadySignalPath(ready_file_.string());

    // Start Server
    grpc::ServerBuilder builder;
    builder.AddListeningPort(uds_path_, grpc::InsecureServerCredentials());
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();

    // Create ready file to unblock client
    std::ofstream ofs(ready_file_);
    ofs << "ready";
    ofs.close();
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
  MockIsolateEzBridgeService service_;
  std::unique_ptr<grpc::Server> server_;
};

TEST_F(IsolateEzBridgeClientTest, PropagatesDeadline) {
  auto& client = IsolateEzBridgeClient::GetInstance();

  grpc::ClientContext context;
  // Set a deadline 1 second in the future
  absl::Time deadline = absl::Now() + absl::Seconds(1);
  context.set_deadline(absl::ToChronoTime(deadline));

  std::string response;
  grpc::Status status = client.IsolateRpcCall(
      &context, "domain", "service", "method", "ez_id", "request", response);

  ASSERT_TRUE(status.ok()) << "RPC failed: " << status.error_message();
  EXPECT_EQ(response, "response");
}

TEST(MockBridgeClient, MockBridgeClient) {
  MockBridgeClient mock_client;
  grpc::ClientContext context;
  std::string response = "response";

  EXPECT_CALL(mock_client,
              IsolateRpcCall(&context, "domain", "service", "method", "ez_id",
                             "request", response))
      .WillOnce(::testing::Return(grpc::Status::OK));

  grpc::Status status = mock_client.IsolateRpcCall(
      &context, "domain", "service", "method", "ez_id", "request", response);
  ASSERT_TRUE(status.ok()) << "RPC failed: " << status.error_message();
  EXPECT_EQ(response, "response");
}

}  // namespace
}  // namespace IsolateEzBridgeSdk

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
