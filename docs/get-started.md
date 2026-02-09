# Get Started with Encrypted Zone SDK

This guide serves as an adoption resource for Encrypted Zone SDK (EZ SDK) users to transform an
existing gRPC implementation of a service defined using protobuf Interface Definition Language (IDL)
into a secure EZ isolate.

## SDK adoption for gRPC services

EZ aims to make adopting the SDK a relatively low effort process. The key steps are to replace your
gprc target with a new bazel target for the SDK and change the namespace of the service
implementation to use the new package. Full steps are outlined below.

### Example Proto Definition

The example sections of each step are based on the following proto example:

```protobuf
syntax = "proto3";
package helloworld;

import "enforcer/v1/options.proto";

option java_multiple_files = true;
option java_outer_classname = "HelloWorldProto";
option java_package = "io.grpc.playground.helloworld";

// The greeting service definition.
service Greeter {
  // Sends a greeting
  rpc SayHello(HelloRequest) returns (HelloReply) {}

  // Bidirectional Streaming greeting
  rpc StreamingSayHello(stream HelloRequest) returns (stream HelloReply) {}
}

// The request message containing the user's name.
message HelloRequest {
  string name = 1;
}

// The response message containing the greetings
message HelloReply {
  string message = 1;
}
```

#### 1. Create an ez_isolate_service_cc target

The SDK uses go templates to generate the isolate-specific SDK glue code used to proxy requests to
their isolate implementations. This is done in the SDK through the macro,
[ez_isolate_service_cc](../tools/plugin/ez_sdk.bzl). To use the SDK, import `ez_isolate_service_cc*`
from `${SDK_BASEDIR}/tools/plugins/ez_sdk.bzl`.

This macro takes in as input:

-   `name`:
    -   name of target, basename of ancillary targets.
-   `proto_basename`:
    -   basename of the protobuf source file. Used as the basename of the generated header files.
-   `protos`:
    -   List of `proto_library` targets
-   `protoc_struct`:
    -   Struct returned from `create_ez_protoc_rule`, with three attributes:
        -   `protoc_rule`:
            -   Bazel rule used to run protoc (defaults to cc_backend_protoc)
        -   `sdk_plugins`:
            -   List of sdk plugins to generate code based on templates supplied to
                create_ez_protoc_rule
        -   `Templates_dir`:
            -   Label to a filegroup containing custom templates. If non-empty, custom proto plugins
                will be generated
-   `cc_protos`:
    -   list of cc_proto_library targets

##### Example

```python
load("//tools/plugin:ez_sdk.bzl", "ez_isolate_service_cc")

proto_library(
    name = "greeter_proto",
    srcs = ["greeter.proto"],
    visibility = ["//visibility:public"],
    deps = ["//enforcer/v1:options_proto"],
)

cpp_grpc_library(
    name = "greeter_grpc_proto",
    protos = [":greeter_proto"],
    visibility = ["//visibility:public"],
    deps = ["//enforcer/v1:options_cc_proto"],
)

cc_proto_library(
    name = "greeter_cc_proto",
    visibility = ["//visibility:public"],
    deps = [
        ":greeter_proto",
    ],
)

ez_isolate_service_cc(
    name = "greeter_service",
    cc_protos = [":greeter_cc_proto"],
    proto_basename = "greeter",
    protos = [":greeter_proto"],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "helloworld",
    srcs = ["helloworld.cc"],
    visibility = ["//visibility:public"],
    deps = [
        ":greeter_grpc_proto",
        ":greeter_service",
    ],
)
```

##### Generated Files

This rule generates a replacement header for `cpp_grpc_library`, containing :

**`<proto_basename>.grpc.pb.h`**

It also generates code for the components of the SDK (for internal/debugging purposes):
`<proto_basename>_server_reader_writer.h` `<proto_basename>_client_reader_writer.h`

#### 2. Modify the Service Implementation

With this `ez_isolate_service_cc` target declared, the only code change required to turn the
implementation of the service to an isolate is:

##### Change the namespace of the `ServerReaderWriter&lt;TResponse, TRequest>` parameter from `grpc` to

`<proto_package_name>`

###### Example

```cpp
#include "examples/helloworld/proto/greeter.grpc.pb.h"

...
using grpc::ServerReaderWriter;
using helloworld::ServerReaderWriter;

class GreeterImpl final : public helloworld::Greeter::Service {
...
```

#### 3. Modify Isolate runner binary

Once the service implementation has been modified, to run the service as an isolate, instead of
manually constructing the `grpc::Server` and running it, construct the SDK provided
`EzIsolateBridgeSdk::IsolateRpcServer` object and pass in a `std::shared_ptr` to the service
implementation. To run the server, call `server.StartIsolateRpcServer()`. `StartIsolateRpcServer()`
takes in an optional `socket_path` to run the server on, it runs on
`/enforcer-isolate-shared/ez-isolate-bridge-uds` by default. **Importantly, regardless of if the
original server is constructed, it is not possible to reach the isolate implementation using the
original service and method name.**

##### Example

```cpp
int main(int argc, char** argv) {
  auto service = std::make_shared<GreeterImpl>();
  std::string socket_path = "/enforcer-isolate-shared/ez-isolate-bridge-uds";
  grpc::ServerBuilder builder;
  builder.AddListeningPort("unix:" + socket_path,
                          grpc::InsecureServerCredentials());
  builder.RegisterService(&(*service));
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << socket_path << "\n";
  server->Wait();
  EzIsolateBridgeSdk::IsolateRpcServer server(service);
  server.StartIsolateRpcServer();
}
```

Below is a full isolate-ified service implementation:

```cpp
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

#include "examples/helloworld/proto/greeter.grpc.pb.h"

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
    std::ofstream output_file("outputfile");
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
  std::string socket_path = "/enforcer-isolate-shared/ez-isolate-bridge-uds";
  server.StartIsolateRpcServer(socket_path);
  return 0;
}
```
