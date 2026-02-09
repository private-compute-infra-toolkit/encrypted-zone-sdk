// Copyright 2025 Google LLC
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

#include <sys/stat.h>
#include <unistd.h>

#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/status.h>

#include "absl/container/flat_hash_map.h"
#include "enforcer/v1/ez_isolate_bridge.grpc.pb.h"
#include "testing/local/mock_enforcer.h"

namespace {
using ::grpc::Status;

using ::enforcer::v1::InvokeEzRequest;
using ::enforcer::v1::InvokeEzResponse;

using ::enforcer::v1::EzIsolateBridge;
using ::enforcer::v1::InvokeIsolateRequest;
using ::enforcer::v1::InvokeIsolateResponse;
}  // namespace

void StreamBridge::Start(const InvokeEzRequest& initial_request) {
  std::cout << "Starting StreamBridge\n";
  const auto& metadata = initial_request.control_plane_metadata();
  const std::string& service_name = metadata.destination_service_name();

  auto it = routing_table_.find(service_name);
  if (it == routing_table_.end()) {
    Close(Status(grpc::StatusCode::UNAVAILABLE,
                 "Service not found: " + service_name));
    return;
  }
  const std::string& target_address = it->second;
  std::cout << "[Bridge] Starting stream to " << service_name << " at "
            << target_address << std::endl;

  auto channel =
      grpc::CreateChannel(target_address, grpc::InsecureChannelCredentials());
  auto stub = EzIsolateBridge::NewStub(channel);

  downstream_context_ = std::make_shared<grpc::ClientContext>();
  downstream_stream_ = stub->StreamInvokeIsolate(downstream_context_.get());
  if (!downstream_stream_) {
    Close(Status(grpc::StatusCode::INTERNAL,
                 "Failed to create downstream stream"));
    return;
  }

  ForwardUpstreamToDownstream(initial_request);
  downstream_reader_thread_ =
      std::thread(&StreamBridge::PumpDownstreamToUpstream, this);
}

void StreamBridge::ForwardUpstreamToDownstream(const InvokeEzRequest& request) {
  if (is_closed_) {
    return;
  }

  InvokeIsolateRequest forwarded_request;
  forwarded_request.mutable_control_plane_metadata()->CopyFrom(
      request.control_plane_metadata());
  forwarded_request.mutable_isolate_input()->CopyFrom(
      request.isolate_request_payload());

  // Use a mutex to prevent concurrent writes if this were called from multiple
  // threads.
  is_writing_.wait(true);
  is_writing_.store(true);
  if (!downstream_stream_->Write(forwarded_request)) {
    std::cerr << "[Bridge] Failed to write to downstream." << std::endl;
    Close(Status(grpc::StatusCode::UNAVAILABLE, "Downstream write failed"));
  }
  is_writing_.store(false);
  is_writing_.notify_one();
}

void StreamBridge::PumpDownstreamToUpstream() {
  InvokeIsolateResponse response;
  // This loop blocks on Read() until a message arrives or the stream breaks.
  while (!is_closed_ && downstream_stream_->Read(&response)) {
    InvokeEzResponse forwarded_response;
    forwarded_response.mutable_control_plane_metadata()->CopyFrom(
        response.control_plane_metadata());
    forwarded_response.mutable_ez_response_payload()->CopyFrom(
        response.isolate_output());

    upstream_handler_->HandleDownstreamResponse(forwarded_response);
  }

  upstream_handler_->NotifyConnectionClosed(Status::OK);
}

void StreamBridge::Close(const Status& status) {
  // Ensure only shutdown once.
  if (is_closed_.exchange(true)) {
    return;
  }

  std::cout << "[Bridge] Closing with status: " << status.error_message()
            << std::endl;

  if (downstream_context_) {
    downstream_context_->TryCancel();
  }

  if (downstream_reader_thread_.joinable()) {
    downstream_reader_thread_.join();
  }

  if (downstream_stream_) {
    (void)downstream_stream_->Finish();
  }
}

void StreamBridge::WritesDone() {
  if (!is_closed_ && downstream_stream_) {
    downstream_stream_->WritesDone();
  }
}
