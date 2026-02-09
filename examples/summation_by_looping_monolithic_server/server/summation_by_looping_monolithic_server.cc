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
#include <vector>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include "examples/summation_by_looping_monolithic_server/proto/summation.grpc.pb.h"

using summation::CloneSimpleAdd;
using summation::IntegerSequenceRequest;
using summation::IntegerSequenceResponse;
using summation::SimpleAdd;
using summation::SimpleAddServerReaderWriter;

class SimpleAddImpl final : public SimpleAdd::Service {
 public:
  grpc::Status IntegerSequence(grpc::ServerContext* context,
                               const IntegerSequenceRequest* request,
                               IntegerSequenceResponse* response) override {
    int64_t start_at = request->start_at();
    int64_t end_at = request->end_at();
    int64_t expected_result = request->expected_result();
    std::cout << "SimpleAdd.IntegerSequence inputs: start_at = " << start_at
              << " end_at = " << end_at
              << " expected_result = " << expected_result << std::endl;

    int64_t result = 0;
    for (int64_t i = start_at; i <= end_at; i++) {
      result += i;
    }
    response->set_sequence_sum(result);
    std::cout << "SimpleAdd.IntegerSequence result = " << result << std::endl;
    if (expected_result != 0 && expected_result != result) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "Expected result mismatch");
    }
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

      std::cout << "SimpleAdd.IntegerSequence inputs: start_at = " << start_at
                << " end_at = " << end_at
                << " expected_result = " << expected_result << std::endl;

      int64_t result = 0;
      for (int64_t i = start_at; i <= end_at; i++) {
        result += i;
      }
      IntegerSequenceResponse response;
      response.set_sequence_sum(result);
      std::cout << "SimpleAdd.IntegerSequence result = " << result << std::endl;
      if (expected_result != 0 && expected_result != result) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Expected result mismatch");
      }
      stream->Write(response);
    }
    return grpc::Status::OK;
  }
};

class CloneSimpleAddImpl final : public CloneSimpleAdd::Service {
 public:
  grpc::Status CloneIntegerSequence(
      grpc::ServerContext* context, const IntegerSequenceRequest* request,
      IntegerSequenceResponse* response) override {
    int64_t start_at = request->start_at();
    int64_t end_at = request->end_at();
    int64_t expected_result = request->expected_result();
    std::cout << "CloneSimpleAdd.CloneIntegerSequence inputs: start_at = "
              << start_at << " end_at = " << end_at
              << " expected_result = " << expected_result << std::endl;

    int64_t result = 0;
    for (int64_t i = start_at; i <= end_at; i++) {
      result += i;
    }
    response->set_sequence_sum(result);
    std::cout << "CloneSimpleAdd.CloneIntegerSequence result = " << result
              << std::endl;
    if (expected_result != 0 && expected_result != result) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "Expected result mismatch");
    }
    return grpc::Status::OK;
  }
};

int main(int argc, char** argv) {
  auto service = std::make_shared<SimpleAddImpl>();
  EzIsolateBridgeSdk::IsolateRpcServer server(service);
  auto clone_service = std::make_shared<CloneSimpleAddImpl>();
  server.AddIsolateRpcService(clone_service);
  std::cout << "Starting (Clone)SimpleAdd server" << std::endl;
  if (argc > 1 && std::string(argv[1]) != "") {
    std::string socket_path = argv[1];
    server.StartIsolateRpcServer(socket_path);
  } else {
    server.StartIsolateRpcServer();
  }
  std::cout << "Exiting SimpleAdd server" << std::endl;
  return 0;
}
