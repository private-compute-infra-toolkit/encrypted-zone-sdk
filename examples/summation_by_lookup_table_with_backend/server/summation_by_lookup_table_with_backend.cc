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

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "examples/summation_by_lookup_table_with_backend/proto/precomputed_backend.grpc.pb.h"
#include "examples/summation_by_lookup_table_with_backend/proto/summation.grpc.pb.h"

using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;
using precomputed_backend::FetchTableRequest;
using precomputed_backend::FetchTableResponse;
using precomputed_backend::PrecomputedBackend;
using summation::IntegerSequenceRequest;
using summation::IntegerSequenceResponse;
using summation::SimpleAdd;
using summation::SimpleAddServerReaderWriter;

class SimpleAddImpl final : public SimpleAdd::Service {
 public:
  SimpleAddImpl() {
    // Choose either Unary or Streaming implementation to fetch the lookup table
    // UnarySimpleAddImpl();
    // StreamingSimpleAddImpl();
  }

  // Unary FetchTable call to PrecomputedBackend
  void UnarySimpleAddImpl() {
    std::unique_ptr<PrecomputedBackend::Stub> precomputed_backend_client =
        PrecomputedBackend::NewStub("playground_example");
    auto bridge_client =
        &IsolateEzBridgeSdk::IsolateEzBridgeClient::GetInstance();
    FetchTableRequest fetch_request;
    FetchTableResponse fetch_response;
    ::grpc::ClientContext client_context;
    // This call should fetch the lookup table memory map
    auto status = precomputed_backend_client->FetchTable(
        &client_context, fetch_request, &fetch_response);
    if (!status.ok()) {
      std::cout << "FetchTable failed with error code: " << status.error_code()
                << std::endl;
    }
    lookup_table_ = bridge_client->ReceiveMemShare<int64_t>();
  }

  // Streaming FetchTable call to PrecomputedBackend
  void StreamingSimpleAddImpl() {
    std::unique_ptr<PrecomputedBackend::Stub> precomputed_backend_client =
        PrecomputedBackend::NewStub("playground_example");
    grpc::ClientContext client_context;
    auto pb_rw =
        precomputed_backend_client->StreamingFetchTable(&client_context);
    FetchTableRequest fetch_request;
    pb_rw->Write(fetch_request);

    FetchTableResponse fetch_response;
    pb_rw->Read(&fetch_response);
    pb_rw->WritesDone();
    auto status = pb_rw->Finish();

    if (!status.ok()) {
      std::cout << "FetchTable failed with error code: " << status.error_code()
                << std::endl;
    }
    auto bridge_client =
        &IsolateEzBridgeSdk::IsolateEzBridgeClient::GetInstance();
    lookup_table_ = bridge_client->ReceiveMemShare<int64_t>();
  }

  grpc::Status IntegerSequence(grpc::ServerContext* context,
                               const IntegerSequenceRequest* request,
                               IntegerSequenceResponse* response) override {
    int64_t start_at = request->start_at();
    int64_t end_at = request->end_at();
    int64_t expected_result = request->expected_result();
    // std::cout << "SimpleAdd.IntegerSequence inputs: start_at = " << start_at
    //           << " end_at = " << end_at
    //           << " expected_result = " << expected_result << std::endl;

    if (start_at >= lookup_table_.size() || end_at >= lookup_table_.size() ||
        start_at < 0 || end_at < 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Out of bounds");
    }
    if (start_at == 0) {
      start_at = 1;  // Starting at 1 is equivaelnt, and simplifies logic
    }
    int64_t result = lookup_table_[end_at] - lookup_table_[start_at - 1];
    response->set_sequence_sum(result);
    // std::cout << "SimpleAdd sequence_sum = " << result << std::endl;
    if (expected_result != 0 && expected_result != result) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "Expected result mismatch");
    }
    // std::cout << "Marking SimpleAdd server IDLE" << std::endl;
    // IsolateEzBridgeSdk::IsolateEzBridgeClient::GetInstance().NewIsolateState(
    //     enforcer::proto::IsolateState::IDLE);
    return grpc::Status::OK;
  }

  grpc::Status StreamingIntegerSequence(
      grpc::ServerContext* context,
      SimpleAddServerReaderWriter<IntegerSequenceResponse,
                                  IntegerSequenceRequest>* stream) override {
    IntegerSequenceRequest request;
    while (stream->Read(&request)) {
      int64_t start_at = request.start_at();
      int64_t end_at = request.end_at();
      int64_t expected_result = request.expected_result();

      if (start_at >= lookup_table_.size() || end_at >= lookup_table_.size() ||
          start_at < 0 || end_at < 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Out of bounds");
      }

      if (start_at == 0) {
        start_at = 1;  // Starting at 1 is equivalent, and simplifies logic
      }
      int64_t result = lookup_table_[end_at] - lookup_table_[start_at - 1];

      IntegerSequenceResponse response;
      response.set_sequence_sum(result);
      // std::cout << "SimpleAdd sequence_sum = " << result << std::endl;
      if (expected_result != 0 && expected_result != result) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Expected result mismatch");
      }
      stream->Write(response);
    }
    return grpc::Status::OK;
  }

 private:
  IsolateEzBridgeSdk::Vec<int64_t> lookup_table_;
};

int main(int argc, char** argv) {
  sleep(1);  // TODO: Temporary hack to let backends start first
  std::cout << "Starting SimpleAdd server with lookup table backend"
            << std::endl;
  auto service = std::make_shared<SimpleAddImpl>();
  EzIsolateBridgeSdk::IsolateRpcServer server(service);
  if (argc > 1 && std::string(argv[1]) != "") {
    std::string socket_path = argv[1];
    server.StartIsolateRpcServer(socket_path);
  } else {
    server.StartIsolateRpcServer();
  }
  std::cout << "Exiting SimpleAdd server with lookup table backend"
            << std::endl;
  return 0;
}
