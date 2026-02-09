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

#ifndef SDK_TESTING_LOCAL_MOCK_ENFORCER_H_
#define SDK_TESTING_LOCAL_MOCK_ENFORCER_H_

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/server_callback.h>
#include <grpcpp/support/status.h>

#include "absl/container/flat_hash_map.h"
#include "enforcer/v1/ez_isolate_bridge.grpc.pb.h"
#include "enforcer/v1/isolate_ez_bridge.grpc.pb.h"

using ::grpc::CallbackServerContext;
using ::grpc::Server;
using ::grpc::ServerBidiReactor;
using ::grpc::ServerBuilder;
using ::grpc::Status;

using ::enforcer::v1::InvokeEzRequest;
using ::enforcer::v1::InvokeEzResponse;
using ::enforcer::v1::IsolateEzBridge;
using ::enforcer::v1::NotifyIsolateStateRequest;
using ::enforcer::v1::NotifyIsolateStateResponse;

using ::enforcer::v1::EzIsolateBridge;
using ::enforcer::v1::InvokeIsolateRequest;
using ::enforcer::v1::InvokeIsolateResponse;

// Interface for the upstream message handler
class UpstreamMessageHandler {
 public:
  virtual ~UpstreamMessageHandler() = default;

  // Method to handle responses coming from downstream
  virtual void HandleDownstreamResponse(const InvokeEzResponse& response) = 0;

  // Method to notify when the connection is closed
  virtual void NotifyConnectionClosed(const Status& status) = 0;
};

// Manages the bidirectional pipe between two gRPC streams for a single RPC.
class StreamBridge {
 public:
  // Takes a pointer to the upstream handler and the routing table.
  StreamBridge(UpstreamMessageHandler* upstream_handler,
               absl::flat_hash_map<std::string, std::string>* routing_table)
      : upstream_handler_(upstream_handler), routing_table_(*routing_table) {}

  ~StreamBridge() {
    if (downstream_reader_thread_.joinable()) {
      downstream_reader_thread_.join();
    }
  }

  // Initiates the connection to the downstream isolate (Isolate B).
  void Start(const InvokeEzRequest& initial_request);

  // Forwards a message from Isolate A -> Isolate B.
  void ForwardUpstreamToDownstream(const InvokeEzRequest& request);

  // Closes the bridge and terminates both streams.
  void Close(const Status& status);

  // Signals that the upstream (Isolate A) is done writing messages.
  void WritesDone();

 private:
  // The loop for the dedicated reader thread.
  void PumpDownstreamToUpstream();

  // The handler for messages going upstream
  UpstreamMessageHandler* upstream_handler_;

  // The client stream for the downstream (Mock Enforcer -> Isolate B)
  // connection.
  std::unique_ptr<
      grpc::ClientReaderWriter<InvokeIsolateRequest, InvokeIsolateResponse>>
      downstream_stream_;

  // A map where the key is the service name (e.g., "MyService") and the value
  // is the target address for that service (e.g.,
  // "/tmp/my-service-server-uds").
  const absl::flat_hash_map<std::string, std::string>& routing_table_;
  std::thread downstream_reader_thread_;
  std::shared_ptr<grpc::ClientContext> downstream_context_;
  std::atomic<bool> is_closed_{false};
  std::atomic<bool> is_writing_{false};
};

// This reactor handles the server-side stream from the initiating isolate
// (Isolate A).
class StreamInvokeEzReactor
    : public ServerBidiReactor<InvokeEzRequest, InvokeEzResponse>,
      public UpstreamMessageHandler {
 public:
  StreamInvokeEzReactor(
      absl::flat_hash_map<std::string, std::string>* routing_table) {
    bridge_ = std::make_unique<StreamBridge>(this, routing_table);
    StartRead(&request_);
  }

  void OnReadDone(bool ok) override {
    if (!ok) {
      bridge_->WritesDone();
      return;
    }

    if (!is_started_) {
      is_started_ = true;
      bridge_->Start(request_);
    } else {
      bridge_->ForwardUpstreamToDownstream(request_);
    }

    StartRead(&request_);
  }

  void OnWriteDone(bool ok) override {}

  void OnDone() override {
    bridge_->Close(grpc::Status::OK);
    delete this;
  }

  void HandleDownstreamResponse(const InvokeEzResponse& response) override {
    StartWrite(&response);
  }

  void NotifyConnectionClosed(const Status& status) override { Finish(status); }

 private:
  std::unique_ptr<StreamBridge> bridge_;
  InvokeEzRequest request_;
  bool is_started_ = false;
};

#endif  // SDK_TESTING_LOCAL_MOCK_ENFORCER_H_
