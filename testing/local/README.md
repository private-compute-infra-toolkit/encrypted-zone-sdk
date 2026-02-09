# test_isolate

A CLI tool for testing Privacy Sandbox EZ isolate services locally. It handles starting up a mock
enforcer, launching isolate servers, and making test gRPC calls.

## Usage

```bash
./test_isolate [options] <input file>
```

The tool accepts either:

-   A JSON config file containing test parameters and requests
-   A textproto file containing request data

### System Requirements

#### Operating System Compatibility

This script runs binaries directly on the host system, which requires compatibility between the host
system's libraries and the binary's dependencies. Specifically:

-   GNU libc (glibc) version 2.31 or later is required
-   The script will check your system's libc version and warn if incompatible
-   For better compatibility across different environments, consider using the
    docker-compose-hermetic test environment instead

#### Required Tools

To use the `test_isolate` script, the following tools must be installed and available in your system
PATH:

-   `jq`: For JSON processing
-   `protoc`: Protocol buffer compiler
-   `grpcurl`: gRPC command-line client
-   `ldd`: For checking dynamic library dependencies

### Options

-   `--manifest, -m`: Path to manifest.json file (required)
-   `--rpc, -r`: RPC name to use in control plane metadata (required)
-   `--rpc_type, -t`: RPC type - "unary" or "streaming" (default: unary)
-   `--output_file, -o`: Output textproto file path (optional)
-   `--debug, -d`: Enable debug mode (skip rebuilding non-server targets)
-   `--verbose`: Produce verbose output
-   `-h, --help`: Show help message

### Input File Formats

#### JSON Config

JSON config files should contain:

```json
{
    "manifest_json_path": "path/to/manifest.json",
    "rpc": "RpcMethodName",
    "rpc_type": "unary",
    "proto_message": "package.MessageType",
    "proto_file": "path/to/proto/file.proto",
    "requests": [
        {
            "field1": "value1",
            "field2": "value2"
        }
    ]
}
```

Requests will first get converted to a 'field1: "value1", field2: "value2"' string, converted to
protobin then base64 encoded in an InvokeIsolateRequest json object. Currently, only simple requests
without nested objects are supported.

#### Manifest JSON

This CLI tool reads the filenames and service names from the manifest JSON provided. It is required
for this tool that the binary manifest is in the same directory as the binaries to be built and run.

#### Textproto

Textproto files must start with proto metadata comments:

```protobuf
# proto-file: "path/to/file.proto"
# proto-message: package.MessageType

field1: "value1"
field2: "value2"
```

### Example Usage

Testing a unary RPC:

```bash
./test_isolate -m examples/helloworld/main/ez_manifest.json \
               -r SayHello \
               testing/docker-compose-hermetic/test_cases/helloworld/ezhello.request.textproto
```

Using a JSON config:

```bash
./test_isolate testing/local/greeter_config.json
```

### Local testing without test_isolate script

To test locally without the test_isolate script, the mock enforcer can be run standalone. It accepts
names of the service to be tested and all dependent services:

```bash
bazel-bin/testing/local/mock_enforcer SimpleAdd ArithmeticBackend
```

The isolate server can be built and run, for example:

```bash
bazel-bin/examples/summation_by_looping_with_backend/server/summation_by_looping_with_backend
```

Allowing us to send InvokeIsolateRequests to the isolate binary using grpcurl.

```bash
grpcurl -d @ -protoset bazel-bin/enforcer/v1/enforcer_descriptor_set.pb -plaintext -unix /enforcer-isolate-shared/ez-isolate-bridge-uds enforcer.v1.EzIsolateBridge/[StreamingInvokeIsolate|InvokeIsolate]<request.json
```

where request.json contains a JSON representation of an InvokeIsolateRequest:

```json
{
    "control_plane_metadata": {
        "destination_service_name": "SimpleAdd",
        "destination_method_name": "IntegerSequence"
    },
    "isolate_input": {
        "datagrams": [
            /*Base64 encoding of request*/
        ]
    }
}
```

The base 64 encoding of your request can be generated using protoc:

```bash
echo "${REQUEST_TEXTPROTO}" | protoc --encode="${REQUEST_TYPE}" \
--descriptor_set_in="${PATH_TO_PROTO_DESCRIPTOR_SET}" | base64
```

## Output

Results are written to:

-   The specified output file if `--output_file` is provided
-   Standard output otherwise

The output is formatted as textproto with the original proto metadata preserved.
