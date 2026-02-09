# Copyright 2025 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

load("@aspect_bazel_lib//lib:diff_test.bzl", "diff_test")
load(
    "@google_privacysandbox_functionaltest_system//bazel:rpc_test.bzl",
    "functional_test_files_for",
)

def encode_textproto(name, textproto_file, descriptor_set):
    """Convert a textproto file to binary using protoc.

    Args:
        name: name for the generated rule
        textproto_file: label of the textproto file to convert
        descriptor_set: label of the protobuf descriptor set
    """

    # Extract message type and convert textproto to binary using protoc
    native.genrule(
        name = name,
        srcs = [textproto_file, descriptor_set],
        outs = [name + "_encoded.json"],
        cmd = """
        MESSAGE_TYPE=$$(grep '# proto-message:' $(location {textproto_file}) | cut -d' ' -f3)
        DESCRIPTOR_SET=$(location {descriptor_set})
        TEXTPROTO_FILE=$(location {textproto_file})

        $${{PROTOC}} --encode=$${{MESSAGE_TYPE}} --descriptor_set_in=$${{DESCRIPTOR_SET}} < $${{TEXTPROTO_FILE}} > $@
        """.format(
            descriptor_set = descriptor_set,
            textproto_file = textproto_file,
        ),
    )

def decode_textproto(name, textproto_file, binary_file, descriptor_set):
    """Convert a binary file to textproto using protoc.

    Args:
        name: name for the generated rule
        binary_file: label of the binary file to convert
        descriptor_set: label of the protobuf descriptor set
    """

    # Extract message type and convert textproto to binary using protoc
    native.genrule(
        name = name,
        srcs = [textproto_file, binary_file, descriptor_set],
        outs = [name + "_decoded.textproto"],
        cmd = """
        MESSAGE_TYPE=$$(grep '# proto-message:' $(location {textproto_file}) | cut -d' ' -f3)
        DESCRIPTOR_SET=$(location {descriptor_set})
        BINARY_FILE=$(location {binary_file})
        cat $${{BINARY_FILE}}

        $${{PROTOC}} --decode=$${{MESSAGE_TYPE}} --descriptor_set_in=$${{DESCRIPTOR_SET}} < $${{BINARY_FILE}} > $@
        """.format(
            descriptor_set = descriptor_set,
            binary_file = binary_file,
            textproto_file = textproto_file,
        ),
    )

def decode_base64(name, file):
    native.genrule(
        name = name + "_decoder",
        srcs = [file],
        outs = [name + "_decoded.json"],
        cmd = "base64 -d $< > $@",
    )

def encode_base64(name, file):
    native.genrule(
        name = name + "_encoder",
        srcs = [file],
        outs = [name + "_encoded.json"],
        cmd = "base64 $< | tr -d '\n' > $@",
    )

def rpc_diff_test_suite(
        name,
        endpoint,
        test_files_glob_spec,
        protoset = "",
        custom_rpc_invoker_tarball = "",
        test_size = "medium",
        test_tags = [],
        plaintext = False,
        jq_raw = False,
        is_streaming_rpc = False,
        method_name = "",
        service_name = "",
        **kwargs):
    """Generate a test suite for test cases within the specified directory tree.

    Args:
      name: test suite name
      test_files_glob_spec: glob spec for test files, passed to function functional_test_files_for()
      protoset: protobuf descriptor set label or file
      custom_rpc_invoker_tarball: label for an image tarball used to invoke rpc requests
      test_tags: tag list for the tests
      plaintext: boolean to indicate plaintext requests
      jq_raw: boolean to indicate raw input/output for jq
      **kwargs: additional args
    """
    test_files = functional_test_files_for(
        glob_spec = test_files_glob_spec,
        request_suffix = ".request.textproto",
        reply_suffix = ".reply.textproto",
    )
    if not test_files:
        print("no test files found for glob spec: ", test_files_glob_spec)
        return

    test_labels = []
    for test_name, testcase_files in test_files.items():
        qual_test_name = "{}-{}".format(name, test_name)
        test_labels.append(":{}".format(qual_test_name))
        extra_kwargs = dict(kwargs)
        if testcase_files["pre-filter"]:
            extra_kwargs["jq_pre_filter"] = ":{}".format(testcase_files["pre-filter"])
        if testcase_files["post-filter"]:
            extra_kwargs["jq_post_filter"] = ":{}".format(testcase_files["post-filter"])
        elif testcase_files["post-filter-slurp"]:
            extra_kwargs["jq_post_filter"] = ":{}".format(testcase_files["post-filter-slurp"])
            extra_kwargs["jq_post_slurp"] = True
        rpc_diff_test(
            name = qual_test_name,
            endpoint = endpoint,
            golden_reply = ":{}".format(testcase_files["reply"]),
            protoset = protoset,
            request = ":{}".format(testcase_files["request"]),
            test_size = test_size,
            tags = test_tags,
            custom_rpc_invoker_tarball = custom_rpc_invoker_tarball,
            plaintext = plaintext,
            jq_raw = jq_raw,
            is_streaming_rpc = is_streaming_rpc,
            method_name = method_name,
            service_name = service_name,
            **extra_kwargs
        )
    native.test_suite(
        name = name,
        tests = test_labels,
        tags = test_tags,
    )

def rpc_diff_test(
        name,
        request,
        golden_reply,
        endpoint,
        protoset = "",
        custom_rpc_invoker_tarball = "",
        jq_pre_filter = "",
        jq_post_filter = "",
        jq_post_slurp = False,
        test_size = "medium",
        plaintext = False,
        client_type = "",
        client_ip = "",
        client_user_agent = "",
        client_accept_language = "",
        jq_raw = False,
        is_streaming_rpc = False,
        method_name = "",
        service_name = "",
        **kwargs):
    """Generates a diff test for a grpc request/reply.

    Args:
      name: test suite name
      request: label of request file
      golden_reply: label of reply file
      endpoint: struct for endpoint defining the protocol, host, port etc
      rpc: gRPC qualified rpc name
      protoset: protobuf descriptor set label or file
      custom_rpc_invoker_tarball: label for an image tarball used to invoke rpc requests
      jq_pre_filter: jq filter program as string to apply to the rpc request
      jq_post_filter: jq filter program as string to apply to the rpc response
      jq_post_slurp: boolean to indicate use of jq --slurp for the rpc response
      plaintext: boolean to indicate plaintext request
      client_type: client type to use for the rpc request
      client_ip: client IP address to use for the rpc request
      client_user_agent: client User-Agent header to use for the rpc request
      client_accept_language: client Accept-Language header to use for the rpc request
      jq_raw: boolean to indicate raw input/output for jq
      **kwargs: additional test args
    """
    runner = Label("@google_privacysandbox_functionaltest_system//bazel:rpc_test_runner")
    rpc_invoker_type = "binary"
    generated_reply_filename = name + "_test_generated_reply.json"
    if is_streaming_rpc:
        rpc = "enforcer.v1.EzPublicApi.StreamCall"
    else:
        rpc = "enforcer.v1.EzPublicApi.Call"
    enforcer_protoset = "//:data/enforcer_descriptor_set.pb"

    encode_textproto(name + "_request_from_textproto", request, protoset)
    encode_base64(name + "_request", ":{}_request_from_textproto_encoded.json".format(name))

    if custom_rpc_invoker_tarball:
        rpc_invoker = custom_rpc_invoker_tarball
        rpc_invoker_type = "image"
    elif endpoint.endpoint_type == "grpc":
        rpc_invoker = Label("@google_privacysandbox_functionaltest_system//bazel:grpcurl_rpc_invoker")
    elif endpoint.endpoint_type == "http":
        rpc_invoker = Label("@google_privacysandbox_functionaltest_system//bazel:curl_rpc_invoker")
    else:
        fail("[rpc_diff_test] unsupported endpoint type:", endpoint.endpoint_type)
    args = [
        "--endpoint-hostport",
        "{}:{}".format(endpoint.host, endpoint.port),
        "--rpc",
        rpc,
        "--protoset",
        "$(rootpath {})".format(enforcer_protoset),
        "--request",
        "$(execpath {})".format(name + "_request_encoded.json"),
        "--rpc-invoker",
        "$(rootpath {})".format(rpc_invoker),
        "--rpc-invoker-type",
        rpc_invoker_type,
        "--jq-pre-args",
        "METHOD_NAME={} SERVICE_NAME={}".format(method_name, service_name),
    ]
    if endpoint.docker_network:
        args.extend([
            "--docker-network",
            endpoint.docker_network,
        ])

    if endpoint.http_path and endpoint.endpoint_type == "http":
        args.extend([
            "--http-path",
            endpoint.http_path,
        ])

    data = [
        ":{}_request_encoded.json".format(name),
        enforcer_protoset,
        rpc_invoker,
    ]

    if plaintext:
        args.extend(["--plaintext"])
    if client_type:
        args.extend(["--client-type", client_type])
    if client_ip:
        args.extend(["--client-ip", client_ip])
    if client_user_agent:
        args.extend(["--client-user-agent", client_user_agent])
    if client_accept_language:
        args.extend(["--client-accept-language", client_accept_language])
    if jq_pre_filter:
        args.extend(["--jq-pre-filter", "$(execpath {})".format(jq_pre_filter)])
        data.append(jq_pre_filter)
    if jq_post_filter:
        args.extend(["--jq-post-filter", "$(execpath {})".format(jq_post_filter)])
        data.append(jq_post_filter)
    if jq_post_slurp:
        args.extend(["--jq-post-slurp"])
    if jq_raw:
        args.extend(["--jq-raw"])

    native.genrule(
        name = name + "_reply_generator",
        srcs = [runner] + data,
        outs = [generated_reply_filename],
        cmd = "$(location %s) %s > $@" % (runner, " ".join(args)),
        testonly = True,
        **kwargs
    )

    encode_textproto(name + "_reply_from_textproto", golden_reply, protoset)
    decode_textproto(name + "_reply_from_binary", golden_reply, ":{}_reply_from_textproto_encoded.json".format(name), protoset)

    # Base64 decode the generated reply
    decode_base64(name + "_generated_reply", ":" + generated_reply_filename)
    decode_textproto(name + "_generated_reply_from_binary", golden_reply, ":{}_generated_reply_decoded.json".format(name), protoset)

    diff_test(
        name = name,
        file1 = ":{}_reply_from_binary_decoded.textproto".format(name),
        file2 = ":{}_generated_reply_from_binary_decoded.textproto".format(name),
        size = test_size,
        **kwargs
    )

def ghz_test(
        name,
        request,
        endpoint,
        protoset,
        jq_pre_filter = "",
        plaintext = False,
        test_size = "medium",
        config_file = None,
        jq_raw = False,
        is_streaming_rpc = False,
        method_name = "",
        service_name = "",
        **kwargs):
    """Generate a ghz report for a grpc request.

    Args:
      name: test suite name
      request: label of request file
      endpoint: struct for endpoint defining the protocol, host, port etc
      rpc: gRPC qualified rpc name
      protoset: protobuf descriptor set label or file
      jq_pre_filter: jq filter program as string to apply to the rpc request
      plaintext: boolean to indicate plaintext request
      jq_raw: boolean to indicate raw output for jq
      config_file: ghz config file
      **kwargs: additional test args
    """
    runner = Label("@google_privacysandbox_functionaltest_system//bazel:ghz_test_runner")
    if endpoint.endpoint_type != "grpc":
        fail("[ghz_test] unsupported endpoint type:", endpoint.endpoint_type)
    if is_streaming_rpc:
        rpc = "enforcer.v1.EzPublicApi.StreamCall"
    else:
        rpc = "enforcer.v1.EzPublicApi.Call"
    enforcer_protoset = "//:data/enforcer_descriptor_set.pb"

    encode_textproto(name + "_request_from_textproto", request, protoset)
    encode_base64(name + "_request", ":{}_request_from_textproto_encoded.json".format(name))

    args = [
        "--endpoint-hostport",
        "{}:{}".format(endpoint.host, endpoint.port),
        "--rpc",
        rpc,
        "--protoset",
        "$(rootpath {})".format(enforcer_protoset),
        "--request",
        "$(execpath {})".format(":" + name + "_request_encoded.json"),
        "--jq-pre-args",
        "METHOD_NAME={} SERVICE_NAME={}".format(method_name, service_name),
    ]
    data = [
        enforcer_protoset,
        ":{}_request_encoded.json".format(name),
    ]

    if endpoint.docker_network:
        args.extend(["--docker-network", endpoint.docker_network])
    if plaintext:
        args.extend(["--plaintext"])
    if jq_raw:
        args.extend(["--jq-raw"])
    if config_file:
        data.append(config_file)
        args.extend(["--config-file", "$(execpath {})".format(config_file)])
    if endpoint.http_path and endpoint.endpoint_type == "http":
        args.extend([
            "--http-path",
            endpoint.http_path,
        ])

    if jq_pre_filter:
        args.extend(["--jq-pre-filter", "$(execpath {})".format(jq_pre_filter)])
        data.append(jq_pre_filter)

    generated_report_filename = name + "_ghz_report.json"

    native.genrule(
        name = name + "_report_generator",
        srcs = [runner] + data,
        outs = [generated_report_filename],
        cmd = "$(location %s) %s > $@" % (runner, " ".join(args)),
        testonly = True,
        **kwargs
    )

    # Create a simple test that verifies the report was generated
    native.sh_test(
        name = name,
        srcs = ["//test_cases:verify_report.sh"],
        args = ["$(location :{})".format(generated_report_filename)],
        data = [":" + generated_report_filename],
        size = test_size,
        **kwargs
    )

def ghz_test_suite(
        name,
        endpoint,
        test_files_glob_spec,
        protoset,
        test_size = "medium",
        test_tags = [],
        plaintext = False,
        jq_raw = False,
        config_file = None,
        is_streaming_rpc = False,
        method_name = "",
        service_name = "",
        **kwargs):
    """Generates a test suite for test cases within the specified directory tree.

    Args:
      name: test suite name
      test_files_glob_spec: glob spec for test files, passed to function functional_test_files_for()
      protoset: protobuf descriptor set label or file
      test_tags: tag list for the tests
      plaintext: boolean to indicate plaintext requests
      jq_raw: boolean to indicate raw output for jq
      config_file: ghz config file
      **kwargs: additional args
    """
    test_files = functional_test_files_for(
        glob_spec = test_files_glob_spec,
        request_suffix = ".request.textproto",
        reply_suffix = ".reply.textproto",
    )
    if not test_files:
        print("no test files found for glob spec: ", test_files_glob_spec)
        return

    test_labels = []
    for test_name, testcase_files in test_files.items():
        qual_test_name = "{}-{}".format(name, test_name)
        test_labels.append(":{}".format(qual_test_name))
        extra_kwargs = dict(kwargs)
        if testcase_files["pre-filter"]:
            extra_kwargs["jq_pre_filter"] = ":{}".format(testcase_files["pre-filter"])
        ghz_test(
            name = qual_test_name,
            endpoint = endpoint,
            protoset = protoset,
            request = ":{}".format(testcase_files["request"]),
            test_size = test_size,
            tags = test_tags,
            plaintext = plaintext,
            jq_raw = jq_raw,
            config_file = config_file,
            is_streaming_rpc = is_streaming_rpc,
            method_name = method_name,
            service_name = service_name,
            **extra_kwargs
        )

    native.test_suite(
        name = name,
        tests = test_labels,
        tags = test_tags,
    )
