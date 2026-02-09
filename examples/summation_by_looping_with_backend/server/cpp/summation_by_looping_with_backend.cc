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

#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "examples/summation_by_looping_with_backend/proto/arithmetic_backend.grpc.pb.h"
#include "examples/summation_by_looping_with_backend/proto/summation.grpc.pb.h"

using arithmetic_backend::AddRequest;
using arithmetic_backend::AddResponse;
using arithmetic_backend::ArithmeticBackend;
using summation::IntegerSequenceRequest;
using summation::IntegerSequenceResponse;
using summation::SimpleAdd;
using summation::SimpleAddServerReaderWriter;

class SimpleAddImpl final : public SimpleAdd::Service {
 public:
  // Unary with Unary call to ArithmeticBackend
  grpc::Status IntegerSequence(grpc::ServerContext* context,
                               const IntegerSequenceRequest* request,
                               IntegerSequenceResponse* response) override {
    int64_t start_at = request->start_at();
    int64_t end_at = request->end_at();
    int64_t expected_result = request->expected_result();
    std::cout << "SimpleAdd.IntegerSequence inputs: start_at = " << start_at
              << " end_at = " << end_at
              << " expected_result = " << expected_result << std::endl;

    // This is lightweight, so we can instantiate a new stub in every handler
    // (but real code probably would have reused a longer lived stub).
    std::unique_ptr<ArithmeticBackend::Stub> arithmetic_client =
        ArithmeticBackend::NewStub("playground_example");
    grpc::ClientContext client_context;  // Not really used
    int64_t result = 0;
    for (int64_t i = start_at; i <= end_at; i++) {
      AddRequest add_request;
      add_request.set_left(result);
      add_request.set_right(i);
      AddResponse add_response;
      grpc::Status response_status =
          arithmetic_client->Add(&client_context, add_request, &add_response);
      if (!response_status.ok()) {
        return grpc::Status(
            grpc::StatusCode::UNAVAILABLE,
            "Backend error: " + response_status.error_message());
      }
      result = add_response.sum();
    }
    response->set_sequence_sum(result);
    if (expected_result != 0 && expected_result != result) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "Expected result mismatch");
    }
    // std::cout << "Marking SimpleAdd server IDLE" << std::endl;
    // bridge_client_->NewIsolateState(enforcer::proto::IsolateState::IDLE);
    return grpc::Status::OK;
  }

  // Streaming with Streaming call to ArithmeticBackend
  grpc::Status StreamingIntegerSequence(
      grpc::ServerContext* context,
      SimpleAddServerReaderWriter<IntegerSequenceResponse,
                                  IntegerSequenceRequest>* stream) override {
    IntegerSequenceRequest request;
    std::unique_ptr<ArithmeticBackend::Stub> arithmetic_client =
        ArithmeticBackend::NewStub("playground_example");
    grpc::ClientContext client_context;  // Not really used
    auto add_rw = arithmetic_client->StreamingAdd(&client_context);

    while (stream->Read(&request)) {
      int64_t start_at = request.start_at();
      int64_t end_at = request.end_at();
      int64_t expected_result = request.expected_result();

      std::cout << "SimpleAdd.IntegerSequence inputs: start_at = " << start_at
                << " end_at = " << end_at
                << " expected_result = " << expected_result << std::endl;
      int64_t result = 0;
      for (int64_t i = start_at; i <= end_at; i++) {
        AddRequest add_request;
        add_request.set_left(result);
        add_request.set_right(i);
        add_rw->Write(add_request);

        AddResponse add_response;
        if (!add_rw->Read(&add_response)) {
          return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                              "Backend error: no response");
        }
        result = add_response.sum();
      }

      IntegerSequenceResponse response;
      response.set_sequence_sum(result);
      if (expected_result != 0 && expected_result != result) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Expected result mismatch");
      }
      stream->Write(response);
    }
    add_rw->WritesDone();
    grpc::Status status = add_rw->Finish();
    if (!status.ok()) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                          "Backend error: " + status.error_message());
    }
    // std::cout << "Marking SimpleAdd server IDLE" << std::endl;
    // bridge_client_->NewIsolateState(enforcer::proto::IsolateState::IDLE);
    return grpc::Status::OK;
  }
};

int main(int argc, char** argv) {
  std::cout << "Starting SimpleAdd server with backend" << std::endl;
  auto service = std::make_shared<SimpleAddImpl>();
  EzIsolateBridgeSdk::IsolateRpcServer server(service);
  if (argc > 1 && std::string(argv[1]) != "") {
    std::string socket_path = argv[1];
    server.StartIsolateRpcServer(socket_path);
  } else {
    server.StartIsolateRpcServer();
  }
  std::cout << "Exiting SimpleAdd server with backend" << std::endl;
  return 0;
}
