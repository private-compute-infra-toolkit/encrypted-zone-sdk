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

#include "mock_enforcer.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/support/server_callback.h>
#include <grpcpp/support/status.h>

#include "absl/container/flat_hash_map.h"
#include "enforcer/v1/ez_isolate_bridge.grpc.pb.h"
#include "enforcer/v1/isolate_ez_bridge.grpc.pb.h"

namespace {
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

class NotifyStateReactor
    : public ServerBidiReactor<NotifyIsolateStateRequest,
                               NotifyIsolateStateResponse> {
 public:
  NotifyStateReactor() { StartRead(&request_); }

  void OnReadDone(bool ok) override {
    if (!ok) {
      std::cout << "[Mock Enforcer] Client disconnected." << std::endl;
      Finish(grpc::Status::OK);
      return;
    }
    std::cout << "[Mock Enforcer] SUCCESS: Received Isolate state update: "
              << request_.new_isolate_state() << std::endl;
    StartWrite(&response_);
  }

  void OnWriteDone(bool ok) override { Finish(grpc::Status::OK); }

  void OnDone() override { delete this; }

 private:
  NotifyIsolateStateRequest request_;
  NotifyIsolateStateResponse response_;
};

std::string kebabCase(const std::string& s) {
  std::string result;
  for (char c : s) {
    if ((std::isupper(c) || c == ' ' || c == '_') && !result.empty() &&
        result.back() != '-') {
      result += '-';
    }
    if (c != ' ' && c != '_') {
      result += std::tolower(c);
    }
  }
  return result;
}

class MockEnforcerImpl final : public IsolateEzBridge::CallbackService {
 public:
  MockEnforcerImpl(const std::vector<std::string>& services) {
    // Socket paths for each Isolate.
    std::cout << "[Mock Enforcer] Routing table initialized." << std::endl;
    for (const std::string& service : services) {
      std::string socket_path =
          "unix:///tmp/" + kebabCase(service) + "-server-uds";
      routing_table_[service] = socket_path;
      std::cout << "[Mock Enforcer]   - " << service << " -> " << socket_path
                << std::endl;
    }
  }

  ServerBidiReactor<NotifyIsolateStateRequest, NotifyIsolateStateResponse>*
  NotifyIsolateState(CallbackServerContext* context) override {
    std::cout << "[Mock Enforcer] Client connected to NotifyIsolateState."
              << std::endl;
    return new NotifyStateReactor();
  }

  grpc::ServerUnaryReactor* InvokeEz(CallbackServerContext* context,
                                     const InvokeEzRequest* request,
                                     InvokeEzResponse* response) override {
    const auto& metadata = request->control_plane_metadata();
    const std::string& service_name = metadata.destination_service_name();
    const std::string& method_name = metadata.destination_method_name();

    std::cout << "[Mock Enforcer] Received InvokeEz request for "
              << service_name << "/" << method_name << std::endl;

    // Look up the destination isolate in our routing table.
    auto it = routing_table_.find(service_name);
    if (it == routing_table_.end()) {
      std::cerr << "[Mock Enforcer] ERROR: Service '" << service_name
                << "' not found in routing table." << std::endl;
      auto* reactor = context->DefaultReactor();
      reactor->Finish(
          Status(grpc::StatusCode::UNAVAILABLE, "Service not found"));
      return reactor;
    }
    const std::string& target_address = it->second;
    std::cout << "[Mock Enforcer] Routing to address: " << target_address
              << std::endl;

    // Act as a gRPC client to call the destination isolate.
    // Create a temporary channel and stub for this call.
    auto channel =
        grpc::CreateChannel(target_address, grpc::InsecureChannelCredentials());
    auto stub = EzIsolateBridge::NewStub(channel);

    InvokeIsolateRequest forwarded_request;
    forwarded_request.mutable_control_plane_metadata()
        ->set_destination_service_name(service_name);
    forwarded_request.mutable_control_plane_metadata()
        ->set_destination_method_name(method_name);
    forwarded_request.mutable_isolate_input()->add_datagrams(
        request->isolate_request_payload().datagrams(0));

    grpc::ClientContext client_context;
    InvokeIsolateResponse forwarded_response;
    Status status = stub->InvokeIsolate(&client_context, forwarded_request,
                                        &forwarded_response);

    if (!status.ok()) {
      std::cerr << "[Mock Enforcer] ERROR: Call to target isolate failed: "
                << status.error_message() << std::endl;
      auto* reactor = context->DefaultReactor();
      reactor->Finish(status);
      return reactor;
    }

    std::cout
        << "[Mock Enforcer] Successfully received response from target isolate."
        << std::endl;
    response->mutable_ez_response_payload()->add_datagrams(
        forwarded_response.isolate_output().datagrams(0));
    response->mutable_control_plane_metadata()->set_ipc_message_id(
        metadata.ipc_message_id());

    auto* reactor = context->DefaultReactor();
    reactor->Finish(Status::OK);
    return reactor;
  }

  grpc::ServerBidiReactor<InvokeEzRequest, InvokeEzResponse>* StreamInvokeEz(
      CallbackServerContext* context) override {
    std::cout << "[Mock Enforcer] Incoming StreamInvokeEz connection."
              << std::endl;
    return new StreamInvokeEzReactor(&routing_table_);
  }

 private:
  absl::flat_hash_map<std::string, std::string> routing_table_;
};

void RunServer(const std::string& socket_path,
               const std::vector<std::string>& services) {
  mkdir("/enforcer-isolate-shared", 0755);
  unlink(socket_path.c_str());

  MockEnforcerImpl service(services);
  ServerBuilder builder;
  builder.AddListeningPort("unix:" + socket_path,
                           grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<Server> server(builder.BuildAndStart());
  if (server == nullptr) {
    std::cerr << "Mock Enforcer server failed to start. Aborting." << std::endl;
    return;
  }

  std::cout << "[Mock Enforcer] Server listening on unix:" << socket_path
            << std::endl;
  server->Wait();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <service1> [service2 ...]"
              << std::endl;
    return 1;
  }

  std::vector<std::string> services;
  for (int i = 1; i < argc; i++) {
    services.push_back(argv[i]);
  }

  const std::string socket_path =
      "/enforcer-isolate-shared/isolate-ez-bridge-uds";
  RunServer(socket_path, services);
  return 0;
}
