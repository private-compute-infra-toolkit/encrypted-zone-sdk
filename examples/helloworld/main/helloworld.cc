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

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include "greeter.grpc.pb.h"

using grpc::Channel;
using grpc::ChannelArguments;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;
using helloworld::Greeter;
using helloworld::GreeterServerReaderWriter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

class GreeterImpl final : public Greeter::Service {
 public:
  Status SayHello(ServerContext* context, const HelloRequest* request,
                  HelloReply* reply) override {
    auto message = "Hello " + request->name();
    // We are also indirectly testing file mounting from EZ to the isolate.
    // If the file cannot be opened, the mounting had failed.
    std::ofstream output_file("/enforcer-isolate-shared/outputfile");
    if (!output_file.is_open()) {
      return Status(StatusCode::NOT_FOUND, "File not found");
    }
    output_file << message;
    output_file.close();
    reply->set_message(message);
    return Status::OK;
  }

  Status StreamingSayHello(
      ServerContext* context,
      GreeterServerReaderWriter<HelloReply, HelloRequest>* stream) override {
    HelloRequest request;
    while (stream->Read(&request)) {
      HelloReply reply;
      reply.set_message("Hello " + request.name());
      stream->Write(reply);
    }
    return Status::OK;
  }
};

int main(int argc, char** argv) {
  auto service = std::make_shared<GreeterImpl>();
  EzIsolateBridgeSdk::IsolateRpcServer server(service);
  if (argc > 1 && std::string(argv[1]) != "") {
    std::string socket_path = argv[1];
    server.StartIsolateRpcServer(socket_path);
  } else {
    server.StartIsolateRpcServer();
  }
  return 0;
}
