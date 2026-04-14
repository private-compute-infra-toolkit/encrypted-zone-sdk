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

#include <grpcpp/client_context.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>
#include <grpcpp/support/sync_stream.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <queue>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "core/cpp/src/create_fileshare_response.h"
#include "core/cpp/src/utils.h"
#include "core/cpp/test/mock_isolate_ez_bridge_client.h"
#include "enforcer/v1/ez_payload.pb.h"
#include "enforcer/v1/isolate_ez_bridge.grpc.pb.h"

namespace IsolateEzBridgeSdk {
namespace {

using ::absl_testing::StatusIs;
using ::enforcer::v1::CreateFileshareRequest;
using ::enforcer::v1::EventTopic;
using ::enforcer::v1::FileshareEvent;
using ::enforcer::v1::InvokeEzRequest;
using ::enforcer::v1::InvokeEzResponse;
using ::enforcer::v1::PublishEventForRequest;
using ::enforcer::v1::PublishEventForResponse;
using ::enforcer::v1::StreamSubscribeToRequest;
using ::enforcer::v1::StreamSubscribeToResponse;
using ::IsolateEzBridgeSdk::IsolateEzBridgeClient;
using ::IsolateEzBridgeSdk::MockBridgeClient;
using ::testing::IsEmpty;
using ::testing::Not;

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

  grpc::Status CreateFileshare(
      grpc::ServerContext* context, const CreateFileshareRequest* request,
      ::enforcer::v1::CreateFileshareResponse* response) override {
    // Create a subdirectory for the fileshare using a UUID-like handle.
    std::string handle =
        "tmp/isolate_ez_bridge_test/" + std::to_string(fileshare_counter_++);
    std::filesystem::create_directories("/" + handle);
    response->set_fileshare_handle(handle);
    return grpc::Status::OK;
  }

  grpc::Status PublishEventFor(grpc::ServerContext* context,
                               const PublishEventForRequest* request,
                               PublishEventForResponse* response) override {
    last_publish_request_ = *request;
    publish_call_count_++;
    return grpc::Status::OK;
  }

  grpc::Status StreamSubscribeTo(
      grpc::ServerContext* context, const StreamSubscribeToRequest* request,
      grpc::ServerWriter<StreamSubscribeToResponse>* writer) override {
    absl::MutexLock lock(mu_);
    stream_active_ = true;
    while (true) {
      mu_.Await(absl::Condition(
          this, &MockIsolateEzBridgeService::EventsAvailableOrStopped));
      if (stop_stream_) break;
      while (!events_to_send_.empty()) {
        StreamSubscribeToResponse event = std::move(events_to_send_.front());
        events_to_send_.pop();
        writer->Write(event);
      }
    }
    stream_active_ = false;
    return grpc::Status::OK;
  }

  void SendFileshareEvent(const std::string& handle,
                          FileshareEvent::FileshareEventType type) {
    absl::MutexLock lock(mu_);
    StreamSubscribeToResponse response;
    response.set_topic(EventTopic::EVENT_TOPIC_FILESHARE);
    response.set_handle(handle);
    response.mutable_fileshare_event()->set_event_type(type);
    events_to_send_.push(std::move(response));
  }

  void StopStream() {
    absl::MutexLock lock(mu_);
    stop_stream_ = true;
  }

  void SetTempDir(std::string dir) { temp_dir_ = std::move(dir); }
  bool EventsAvailableOrStopped() const {
    return !events_to_send_.empty() || stop_stream_;
  }

  const PublishEventForRequest& last_publish_request() const {
    return last_publish_request_;
  }
  int publish_call_count() const { return publish_call_count_; }

 private:
  std::string temp_dir_;
  int fileshare_counter_ = 0;
  PublishEventForRequest last_publish_request_;
  int publish_call_count_ = 0;

  absl::Mutex mu_;
  std::queue<StreamSubscribeToResponse> events_to_send_;
  bool stop_stream_ = false;
  bool stream_active_ = false;
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
    service_.SetTempDir(temp_dir_);
    server_ = builder.BuildAndStart();

    // Create ready file to unblock client
    std::ofstream ofs(ready_file_);
    ofs << "ready";
    ofs.close();
  }

  void TearDown() override {
    service_.StopStream();
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

TEST_F(IsolateEzBridgeClientTest, CreateFileshareSuccess) {
  auto& client = IsolateEzBridgeClient::GetInstance();

  CreateFileshareResponse response = client.CreateFileshare();

  ASSERT_OK(response.status);
  EXPECT_THAT(response.fileshare_handle, Not(IsEmpty()));
  EXPECT_FALSE(response.fileshare_handle.empty());
  EXPECT_FALSE(response.shared_path.empty());
  EXPECT_FALSE(response.staging_path.empty());
  // Verify files were actually created
  EXPECT_TRUE(std::filesystem::exists(response.shared_path));
  EXPECT_TRUE(std::filesystem::exists(response.staging_path));

  // Verify file sizes are 0
  EXPECT_EQ(std::filesystem::file_size(response.shared_path), 0);
  EXPECT_EQ(std::filesystem::file_size(response.staging_path), 0);
}

TEST_F(IsolateEzBridgeClientTest, CommitFileChangesSuccess) {
  auto& client = IsolateEzBridgeClient::GetInstance();

  CreateFileshareResponse fs = client.CreateFileshare();
  ASSERT_OK(fs.status);

  // Write content to staging file
  const std::string content = "test content";
  {
    std::ofstream ofs(fs.staging_path);
    ofs << content;
  }
  ASSERT_EQ(std::filesystem::file_size(fs.staging_path), content.size());

  // Commit changes
  absl::Status status = client.CommitFileChanges(fs.staging_path);
  ASSERT_OK(status) << status;

  // Verify that shared file has the content
  {
    std::ifstream ifs(fs.shared_path);
    std::string actual_content((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
    EXPECT_EQ(actual_content, content);
  }

  // Verify that staging file is not deleted and still has the original content.
  {
    std::ifstream ifs(fs.staging_path);
    std::string staging_content((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
    EXPECT_EQ(staging_content, content);
  }

  // Verify that PublishEventFor was called.
  EXPECT_EQ(service_.publish_call_count(), 1);
  EXPECT_EQ(service_.last_publish_request().topic(),
            EventTopic::EVENT_TOPIC_FILESHARE);
  EXPECT_EQ(service_.last_publish_request().handle(),
            std::filesystem::path(fs.shared_path).parent_path().filename());
  EXPECT_EQ(service_.last_publish_request().fileshare_event().event_type(),
            FileshareEvent::FILESHARE_EVENT_TYPE_FILE_UPDATED);

  // Second commit
  const std::string content2 = "second test content";
  {
    std::ofstream ofs(fs.staging_path);
    ofs << content2;
  }

  status = client.CommitFileChanges(fs.staging_path);
  ASSERT_OK(status) << status;

  {
    std::ifstream ifs(fs.shared_path);
    std::string actual_content((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
    EXPECT_EQ(actual_content, content2);
  }

  EXPECT_EQ(service_.publish_call_count(), 2);
}

TEST_F(IsolateEzBridgeClientTest, FileshareEventHandlerCalled) {
  auto& client = IsolateEzBridgeClient::GetInstance();

  absl::Notification notification;
  std::string received_handle;
  FileshareEvent::FileshareEventType received_type;

  client.RegisterFileshareEventHandler(
      [&received_handle, &received_type, &notification](
          absl::string_view handle, FileshareEvent::FileshareEventType type) {
        received_handle = std::string(handle);
        received_type = type;
        notification.Notify();
      });

  const std::string test_handle = "test_handle";
  FileshareEvent::FileshareEventType test_type =
      FileshareEvent::FILESHARE_EVENT_TYPE_FILE_UPDATED;

  service_.SendFileshareEvent(test_handle, test_type);

  ASSERT_TRUE(
      notification.WaitForNotificationWithTimeout(absl::Milliseconds(20)));
  EXPECT_EQ(received_handle, test_handle);
  EXPECT_EQ(received_type, test_type);

  client.ClearFileshareEventHandlers();
}

TEST_F(IsolateEzBridgeClientTest, CommitFileChangesInvalidSuffix) {
  auto& client = IsolateEzBridgeClient::GetInstance();
  absl::Status status = client.CommitFileChanges("invalid_path");
  EXPECT_THAT(status, StatusIs(absl::StatusCode::kInvalidArgument));
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
