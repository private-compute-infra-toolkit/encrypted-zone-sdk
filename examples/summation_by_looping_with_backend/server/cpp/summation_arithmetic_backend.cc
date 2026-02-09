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
#include <grpcpp/support/status.h>

#include "examples/summation_by_looping_with_backend/proto/arithmetic_backend.grpc.pb.h"

using arithmetic_backend::AddRequest;
using arithmetic_backend::AddResponse;
using arithmetic_backend::ArithmeticBackend;
using arithmetic_backend::ArithmeticBackendServerReaderWriter;

class ArithmeticBackendImpl final : public ArithmeticBackend::Service {
 public:
  grpc::Status Add(grpc::ServerContext* context, const AddRequest* request,
                   AddResponse* response) override {
    response->set_sum(request->left() + request->right());
    return grpc::Status::OK;
  }

  grpc::Status StreamingAdd(
      grpc::ServerContext* context,
      ArithmeticBackendServerReaderWriter<AddResponse, AddRequest>* stream)
      override {
    AddRequest request;
    while (stream->Read(&request)) {
      AddResponse response;
      response.set_sum(request.left() + request.right());
      stream->Write(response);
    }
    return Status::OK;
  }
};

int main(int argc, char** argv) {
  std::cout << "Starting ArithmeticBackend server" << std::endl;
  auto service = std::make_shared<ArithmeticBackendImpl>();
  EzIsolateBridgeSdk::IsolateRpcServer server(service);
  if (argc > 1 && std::string(argv[1]) != "") {
    std::string socket_path = argv[1];
    server.StartIsolateRpcServer(socket_path);
  } else {
    server.StartIsolateRpcServer();
  }
  std::cout << "Exiting ArithmeticBacknd server" << std::endl;
  return 0;
}
