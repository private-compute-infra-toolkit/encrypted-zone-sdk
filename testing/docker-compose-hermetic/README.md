# Docker Compose Hermetic Test Runner

This directory contains a test runner that executes hermetic functional tests for Privacy Sandbox EZ
isolates using Docker Compose.

## Overview

The `run-test` script automates the process of:

1. Deploying the enforcer and functionaltest-system containers\*
2. Running functional tests against isolates
3. Collecting and validating test results

\*As run-test relies on the enforcer, the enforcer must be built in the /node repo, and the path to
the built tar file must be specified to run-test.

Example:

```bash
NODE_REPO_PATH=~/node
(cd ${NODE_REPO_PATH} && bazel build //enforcer:copy_to_dist)
testing/docker-compose-hermetic/run-test --enforcer-tar-path ${NODE_REPO_PATH}/dist/enforcer/enforcer_oci.tar
```

## Usage

```bash
./run-test [options] --enforcer-tar-path <path>

Options:
  --test-case | -t     Run a specific test case (can be specified multiple times)
  --rpc-type | -r      Run specific RPC type: unary, streaming (can be specified multiple times)
  --skip-perf-tests    Skip performance tests (specify 'unary' or 'streaming')
  --skip-load-image    Skip building and loading functionaltest image
  --enforcer-tar-path  Path to enforcer tar file (required)
  --verbose            Enable verbose output
```

### Environment Variables

-   `EXTRA_DOCKER_BUILDX_ARGS`: Additional arguments passed to `docker buildx build`

## Adding a New Test Case

To add tests for a new isolate, follow these steps:

1. Create a new directory under `test_cases/` with your test case name

2. Add your test configuration to `test_cases/test_configs.json`:

    ```json
    {
        "your_test_case": {
            "server_bundle_target": "//path/to:your_bundle_target",
            "proto_descriptor_set": "path/to/descriptor.pb",
            "manifest_json_path": "path/to/manifest.json",
            "rpcs": [
                {
                    "method_name": "YourMethod",
                    "service_name": "your.package.YourService",
                    "rpc_type": "unary" // or "streaming"
                }
            ]
        }
    }
    ```

3. Add test files in your test case directory:
    - `*.filter.jq`: JQ filters for response validation
    - `*.pre-filter.jq`: Pre-processing filters (optional)
    - `*.request.textproto`: Request protobuf in text format
    - `*.reply.textproto`: Expected reply protobuf in text format

### Test Configuration Fields

Targets and paths must be relative to the SDK root.

-   `server_bundle_target`: Bazel target for your server bundle
-   `proto_descriptor_set`: Name of the proto descriptor set file
-   `manifest_json_path`: Path to the EZ manifest JSON file
-   `rpcs`: Array of RPC configurations:
    -   `method_name`: Name of the RPC method
    -   `service_name`: Name of Service
    -   `rpc_type`: Either "unary" or "streaming"

## Output and Artifacts

Test outputs are written to a tempXXXXXX directory in dist/tests/testing/docker-compose-hermetic.
This temp\_ directory contains

-   Logs: `tempXXXXXX/${container}-sut.log`
-   The requests and responses used: `tempXXXXXX/sutXXXXXX/tmp/`
-   Test cases: `tempXXXXXX/sutXXXXXX/test_cases/`
