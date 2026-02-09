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

#include "core/cpp/src/ez_isolate_bridge_server.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "core/cpp/src/isolate_ez_bridge_client.h"
#include "core/cpp/test/mock_isolate_rpc_service.h"
#include "enforcer/v1/ez_isolate_bridge.grpc.pb.h"
#include "enforcer/v1/ez_payload.pb.h"
#include "enforcer/v1/isolate_bridge.pb.h"

namespace EzIsolateBridgeSdk {
namespace {

using ::enforcer::v1::InvokeIsolateRequest;
using ::enforcer::v1::InvokeIsolateResponse;
using ::EzIsolateBridgeSdk::MockIsolateRpcService;
using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::IsFalse;
using ::testing::IsTrue;
using ::testing::Return;
using ::testing::SetArgReferee;

using ::enforcer::v1::EzIsolateBridge;
using ::enforcer::v1::IsolateState;
using ::enforcer::v1::UpdateIsolateStateRequest;
using ::enforcer::v1::UpdateIsolateStateResponse;

class EzIsolateBridgeServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create temporary directory for sockets
    temp_dir_ = std::filesystem::temp_directory_path() /
                "ez_isolate_bridge_server_test";
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

    // Start a dummy UDS server
    mock_service_ = std::make_shared<MockIsolateRpcService>();

    // Set expectation for GetServiceName as it is called during registration
    EXPECT_CALL(*mock_service_, GetServiceName())
        .WillRepeatedly(Return("MockService"));

    bridge_ = std::make_shared<EzIsolateBridgeImpl>(mock_service_);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(uds_path_, grpc::InsecureServerCredentials());
    builder.RegisterService(bridge_.get());
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
  std::unique_ptr<grpc::Server> server_;
  std::shared_ptr<MockIsolateRpcService> mock_service_;
  std::shared_ptr<EzIsolateBridgeImpl> bridge_;
};

TEST_F(EzIsolateBridgeServerTest, UpdateIsolateStateStartingToReady) {
  grpc::CallbackServerContext context;
  UpdateIsolateStateRequest request;
  request.set_move_to_state(IsolateState::ISOLATE_STATE_STARTING);
  UpdateIsolateStateResponse response;
  (void)bridge_->UpdateIsolateState(&context, &request, &response);
  EXPECT_EQ(response.current_state(), IsolateState::ISOLATE_STATE_READY);
}

TEST_F(EzIsolateBridgeServerTest, UpdateIsolateStateSimpleTransition) {
  grpc::CallbackServerContext context;
  UpdateIsolateStateRequest request;
  request.set_move_to_state(IsolateState::ISOLATE_STATE_RETIRING);
  UpdateIsolateStateResponse response;
  (void)bridge_->UpdateIsolateState(&context, &request, &response);
  EXPECT_EQ(response.current_state(), IsolateState::ISOLATE_STATE_RETIRING);
}

TEST_F(EzIsolateBridgeServerTest, InvokeIsolateFailsWhenNotReady) {
  auto channel =
      grpc::CreateChannel(uds_path_, grpc::InsecureChannelCredentials());
  auto stub = EzIsolateBridge::NewStub(channel);

  grpc::ClientContext context;
  InvokeIsolateRequest request;
  InvokeIsolateResponse response;
  grpc::Status status = stub->InvokeIsolate(&context, request, &response);

  EXPECT_THAT(status.error_message(),
              ::testing::HasSubstr("Isolate is not ready"));
}

TEST_F(EzIsolateBridgeServerTest, InvokeIsolateSucceedsWhenReady) {
  // 1. Transition to READY
  grpc::CallbackServerContext ctx;
  UpdateIsolateStateRequest update_req;
  update_req.set_move_to_state(IsolateState::ISOLATE_STATE_READY);
  UpdateIsolateStateResponse update_resp;
  (void)bridge_->UpdateIsolateState(&ctx, &update_req, &update_resp);
  ASSERT_EQ(update_resp.current_state(), IsolateState::ISOLATE_STATE_READY);

  // 2. Prepare Request
  auto channel =
      grpc::CreateChannel(uds_path_, grpc::InsecureChannelCredentials());
  auto stub = EzIsolateBridge::NewStub(channel);

  grpc::ClientContext context;
  InvokeIsolateRequest request;
  request.mutable_control_plane_metadata()->set_destination_service_name(
      "MockService");
  request.mutable_control_plane_metadata()->set_destination_method_name(
      "MockMethod");
  request.mutable_isolate_input()->add_datagrams("test_data");

  // 3. Setup Expectation
  std::string response_data = "response_data";
  EXPECT_CALL(*mock_service_, IsolateRpcMethodHandler("MockMethod", _, _, _))
      .WillOnce(testing::DoAll(testing::SetArgReferee<2>(response_data),
                               Return(grpc::Status::OK)));

  // 4. Call InvokeIsolate
  InvokeIsolateResponse response;
  grpc::Status status = stub->InvokeIsolate(&context, request, &response);

  // 5. Verify
  EXPECT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(response.isolate_output().datagrams_size(), 1);
  EXPECT_EQ(response.isolate_output().datagrams(0), response_data);
}

TEST_F(EzIsolateBridgeServerTest, StreamInvokeIsolateFailsWhenNotReady) {
  // 1. Prepare Request without transitioning to READY
  auto channel =
      grpc::CreateChannel(uds_path_, grpc::InsecureChannelCredentials());
  auto stub = EzIsolateBridge::NewStub(channel);

  grpc::ClientContext context;
  auto stream = stub->StreamInvokeIsolate(&context);

  InvokeIsolateRequest request;
  request.mutable_control_plane_metadata()->set_destination_service_name(
      "MockService");
  request.mutable_control_plane_metadata()->set_destination_method_name(
      "MockMethod");
  request.mutable_isolate_input()->add_datagrams("test_data");

  EXPECT_TRUE(stream->Write(request));

  InvokeIsolateResponse response;
  EXPECT_FALSE(stream->Read(&response));
  grpc::Status status = stream->Finish();
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_THAT(status.error_message(),
              ::testing::HasSubstr("Isolate is not ready"));
}

TEST_F(EzIsolateBridgeServerTest, StreamInvokeIsolateSucceedsWhenReady) {
  // 1. Transition to READY
  grpc::CallbackServerContext ctx;
  UpdateIsolateStateRequest update_req;
  update_req.set_move_to_state(IsolateState::ISOLATE_STATE_READY);
  UpdateIsolateStateResponse update_resp;
  (void)bridge_->UpdateIsolateState(&ctx, &update_req, &update_resp);
  ASSERT_EQ(update_resp.current_state(), IsolateState::ISOLATE_STATE_READY);

  // 2. Prepare Request
  auto channel =
      grpc::CreateChannel(uds_path_, grpc::InsecureChannelCredentials());
  auto stub = EzIsolateBridge::NewStub(channel);

  grpc::ClientContext context;
  auto stream = stub->StreamInvokeIsolate(&context);

  InvokeIsolateRequest request;
  request.mutable_control_plane_metadata()->set_destination_service_name(
      "MockService");
  request.mutable_control_plane_metadata()->set_destination_method_name(
      "MockMethod");
  request.mutable_isolate_input()->add_datagrams("test_data");

  // 3. Setup Expectation
  std::string response_data = "response_data";
  EXPECT_CALL(*mock_service_, IsolateStreamRpcMethodHandler(_, _, _, _))
      .WillOnce(testing::Invoke(
          [&](uintptr_t stream_id, const InvokeIsolateRequest& req,
              ResponseWriter* writer,
              std::shared_ptr<std::vector<std::string>> shared_memory) {
            auto response = std::make_unique<InvokeIsolateResponse>();
            response->mutable_isolate_output()->add_datagrams(response_data);
            writer->WriteResponse(std::move(response));
            writer->FinishStream(grpc::Status::OK);
            return grpc::Status::OK;
          }));

  // 4. Write Request
  EXPECT_TRUE(stream->Write(request));

  // 5. Read Response
  InvokeIsolateResponse response;
  EXPECT_TRUE(stream->Read(&response));
  EXPECT_EQ(response.isolate_output().datagrams(0), response_data);

  // 6. Finish
  stream->WritesDone();
  grpc::Status status = stream->Finish();
  EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST(EzIsolateBridgeServerInternalTest, CreateInvalidArgumentResponse) {
  InvokeIsolateResponse response;
  uint64_t ipc_msg_id = 12345;
  std::string message = "test error message";

  CreateInvalidArgumentResponse(message, ipc_msg_id, response);

  EXPECT_THAT(response.status().code(), Eq(grpc::StatusCode::INVALID_ARGUMENT));
  EXPECT_THAT(response.status().message(), Eq(message));
  EXPECT_THAT(response.control_plane_metadata().ipc_message_id(),
              Eq(ipc_msg_id));
}

TEST(EzIsolateBridgeServerInternalTest, ForwardRequestSuccess) {
  auto mock_service = std::make_shared<MockIsolateRpcService>();
  InvokeIsolateRequest request;
  InvokeIsolateResponse response;

  request.mutable_control_plane_metadata()->set_destination_service_name(
      "test_service");
  request.mutable_control_plane_metadata()->set_destination_method_name(
      "test_method");
  request.mutable_control_plane_metadata()->set_ipc_message_id(67890);
  request.mutable_isolate_input()->add_datagrams("request_data");

  EXPECT_CALL(*mock_service, GetServiceName())
      .WillRepeatedly(Return("test_service"));
  EXPECT_CALL(*mock_service, IsolateRpcMethodHandler("test_method", _, _, _))
      .WillOnce(
          DoAll(SetArgReferee<2>("response_data"), Return(grpc::Status::OK)));

  ForwardRequest(mock_service, request, response);

  EXPECT_THAT(response.control_plane_metadata().ipc_message_id(), Eq(67890));
  EXPECT_THAT(response.control_plane_metadata().responder_is_local(), IsTrue());
  EXPECT_THAT(response.isolate_output().datagrams(0), Eq("response_data"));
  EXPECT_THAT(response.has_status(), IsFalse());
}

TEST(EzIsolateBridgeServerInternalTest, ForwardRequestMismatchedService) {
  auto mock_service = std::make_shared<MockIsolateRpcService>();
  InvokeIsolateRequest request;
  InvokeIsolateResponse response;

  request.mutable_control_plane_metadata()->set_destination_service_name(
      "wrong_service");
  request.mutable_control_plane_metadata()->set_ipc_message_id(67890);

  EXPECT_CALL(*mock_service, GetServiceName())
      .WillRepeatedly(Return("test_service"));

  ForwardRequest(mock_service, request, response);

  EXPECT_THAT(response.status().code(), Eq(grpc::StatusCode::INVALID_ARGUMENT));
  EXPECT_THAT(response.status().message(),
              HasSubstr("Mismatched service name"));
}

TEST(EzIsolateBridgeServerInternalTest, ForwardRequestMissingDatagram) {
  auto mock_service = std::make_shared<MockIsolateRpcService>();
  InvokeIsolateRequest request;
  InvokeIsolateResponse response;

  request.mutable_control_plane_metadata()->set_destination_service_name(
      "test_service");
  request.mutable_control_plane_metadata()->set_ipc_message_id(67890);
  // No datagrams added

  EXPECT_CALL(*mock_service, GetServiceName())
      .WillRepeatedly(Return("test_service"));

  ForwardRequest(mock_service, request, response);

  EXPECT_THAT(response.status().code(), Eq(grpc::StatusCode::INVALID_ARGUMENT));
  EXPECT_THAT(response.status().message(), HasSubstr("Missing datagram"));
}

TEST(EzIsolateBridgeServerInternalTest, ForwardRequestServiceError) {
  auto mock_service = std::make_shared<MockIsolateRpcService>();
  InvokeIsolateRequest request;
  InvokeIsolateResponse response;

  request.mutable_control_plane_metadata()->set_destination_service_name(
      "test_service");
  request.mutable_control_plane_metadata()->set_destination_method_name(
      "test_method");
  request.mutable_control_plane_metadata()->set_ipc_message_id(67890);
  request.mutable_isolate_input()->add_datagrams("request_data");

  EXPECT_CALL(*mock_service, GetServiceName())
      .WillRepeatedly(Return("test_service"));
  EXPECT_CALL(*mock_service, IsolateRpcMethodHandler(_, _, _, _))
      .WillOnce(Return(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "method not found")));

  ForwardRequest(mock_service, request, response);

  EXPECT_THAT(response.status().code(), Eq(grpc::StatusCode::NOT_FOUND));
  EXPECT_THAT(response.status().message(), Eq("method not found"));
}

}  // namespace
}  // namespace EzIsolateBridgeSdk

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
