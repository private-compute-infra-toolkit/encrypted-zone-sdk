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

#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include "core/cpp/src/mem_share_response.h"
#include "examples/summation_by_lookup_table_with_backend/proto/precomputed_backend.grpc.pb.h"
#include "examples/summation_by_lookup_table_with_backend/proto/precomputed_backend_client_reader_writer.h"

using grpc::ServerContext;
using grpc::Status;
using precomputed_backend::FetchTableRequest;
using precomputed_backend::FetchTableResponse;
using precomputed_backend::PrecomputedBackend;
using precomputed_backend::PrecomputedBackendServerReaderWriter;

class PrecomputedBackendImpl final : public PrecomputedBackend::Service {
 public:
  PrecomputedBackendImpl(const std::string& shared_memory_handle)
      : shared_memory_handle_(shared_memory_handle) {}

  Status IsolateRpcMethodHandler(
      const std::string& method_name,
      const ::google::protobuf::RepeatedPtrField<std::string>& request_bytes,
      std::string& response_bytes,
      std::vector<std::string>& response_shared_memory_handles) override {
    Status status = PrecomputedBackend::Service::IsolateRpcMethodHandler(
        method_name, request_bytes, response_bytes,
        response_shared_memory_handles);
    if (method_name == "FetchTable") {
      response_shared_memory_handles.push_back(shared_memory_handle_);
    }
    return status;
  }

  Status IsolateStreamRpcMethodHandler(
      uintptr_t stream_id, const InvokeIsolateRequest& invoke_isolate_request,
      EzIsolateBridgeSdk::ResponseWriter* invoke_isolate_resp_writer,
      std::shared_ptr<std::vector<std::string>> response_shared_memory_handles)
      override {
    Status status = PrecomputedBackend::Service::IsolateStreamRpcMethodHandler(
        stream_id, invoke_isolate_request, invoke_isolate_resp_writer,
        response_shared_memory_handles);
    const auto& metadata = invoke_isolate_request.control_plane_metadata();
    const std::string& method_name = metadata.destination_method_name();

    if (method_name == "FetchTable") {
      response_shared_memory_handles->push_back(shared_memory_handle_);
    }
    return status;
  }

  Status FetchTable(ServerContext* context, const FetchTableRequest* request,
                    FetchTableResponse* response) override {
    return Status::OK;
  }

  Status StreamingFetchTable(
      grpc::ServerContext* context,
      PrecomputedBackendServerReaderWriter<
          FetchTableResponse, FetchTableRequest>* stream) override {
    FetchTableRequest request;
    while (stream->Read(&request)) {
      FetchTableResponse response;
      stream->Write(response);
    }
    return Status::OK;
  }

 private:
  std::shared_ptr<IsolateEzBridgeSdk::IsolateEzBridgeClient> client_;
  const std::string& shared_memory_handle_;
};

void precompute_table(IsolateEzBridgeSdk::Vec<int64_t>& lookup_table) {
  for (size_t i = 1; i < lookup_table.size(); i++) {
    lookup_table[i] = lookup_table[i - 1] + i;
  }
}

int main(int argc, char** argv) {
  // 1M of table entries * 8 bytes per entry = 8MB
  uint64_t table_size = (uint64_t)(1024 * 1024);

  std::cout << "Starting PrecomputedBackend server" << std::endl;

  // Create a shared memory region
  auto bridge_client =
      &IsolateEzBridgeSdk::IsolateEzBridgeClient::GetInstance();
  // TODO: Change enforcer so that this line is safe to remove
  bridge_client->NewIsolateState(
      enforcer::v1::IsolateState::ISOLATE_STATE_READY);
  IsolateEzBridgeSdk::Vec<int64_t> lookup_table;
  MemShareResponse mem_share_response =
      bridge_client->CreateMemShare<int64_t>(table_size, lookup_table);
  if (!mem_share_response.status.ok()) {
    std::cout << "Failed to CreateMemShare: Error code = "
              << mem_share_response.status.error_code() << std::endl;
    return 1;
  }

  std::cout << "Created shared memory, initializing it..." << std::endl;
  precompute_table(lookup_table);

  std::cout << "Shared memory initialized, starting server..." << std::endl;
  auto service = std::make_shared<PrecomputedBackendImpl>(
      mem_share_response.shared_memory_handle);
  EzIsolateBridgeSdk::IsolateRpcServer server(service);
  if (argc > 1 && std::string(argv[1]) != "") {
    std::string socket_path = argv[1];
    server.StartIsolateRpcServer(socket_path);
  } else {
    server.StartIsolateRpcServer();
  }
  std::cout << "Exiting PrecomputedBackend server" << std::endl;
  return 0;
}
