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

"""Macro for the EZ Isolate SDK."""

load("@rules_cc//cc:defs.bzl", "cc_library")
load("//tools/plugin/ez_protoc:ez_plugin.bzl", "ez_plugin")
load(
    "//tools/plugin/ez_protoc:filter_files.bzl",
    "filter_files",
)
load(
    "//tools/plugin/internal:ez_sdk.bzl",
    "cc_backend_protoc",
    "cc_backend_template_plugins",
    "default_templates_dir",
    "ez_sdk_backend_protoc",
)

_isolate_service_deps = [
    Label("//core/cpp:ez_isolate_bridge"),
]

def _custom_templates_proto_plugin(*, name, templates_dir, sdk_plugins):
    for plugin in sdk_plugins:
        ez_plugin(
            name = plugin.name,
            outputs = plugin.outputs,
            templates = Label(templates_dir),
            visibility = ["//visibility:public"],
        )

def ez_isolate_service(
        *,
        name,
        proto_basename,
        protos,
        protoc_struct = struct(
            protoc_rule = cc_backend_protoc,
            sdk_plugins = cc_backend_template_plugins,
            templates_dir = default_templates_dir,
        ),
        cc_protos = [],
        **kwargs):
    """
    Top-level macro generating C++ library target for services defined in EZ Isolate SDK proto files.

    Args:
        name: name of target, basename of ancillary targets.
        proto_basename: basename of the protobuf source file. Used as the basename of the generated header files.
        protos: list of proto_library targets
        protoc_struct: Struct returned from create_ez_protoc_rule, with three attributes:
          protoc_rule: Bazel rule used to run protoc (defaults to cc_backend_protoc)
          sdk_plugins: List of sdk plugins to generate code based on templates supplied to create_ez_protoc_rule
          templates_dir: Label to a filegroup containing custom templates. If non-empty, custom proto plugins will be generated
        cc_protos: list of cc_proto_library targets
        **kwargs: attributes for cc_library and those common to bazel build rules.

    Generates:
        <proto_basename>.grpc.pb.h

    Targets:
        <name> -- cc_library
        <name>_cc_hdrs -- c++ header files

    Returns:
        Providers:
            - EzPluginInfo
            - DefaultInfo
    """
    contains_cpp_src_file = False
    for plugin in protoc_struct.sdk_plugins:
        if plugin.option.endswith("cpp") or plugin.option.endswith("cc"):
            contains_cpp_src_file = True
            break

    if protoc_struct.templates_dir != default_templates_dir:
        _custom_templates_proto_plugin(
            name = name,
            templates_dir = protoc_struct.templates_dir,
            sdk_plugins = protoc_struct.sdk_plugins,
        )
    elif len(cc_protos) == 0:
        fail("cc_protos is a required argument for default templates.")

    name_proto = name + "_proto_cc_plugin"
    tags = kwargs.pop("tags", [])
    if "manual" not in tags:
        tags.append("manual")
    ez_sdk_backend_protoc(
        name = name_proto,
        protos = protos,
        proto_basename = proto_basename,
        protoc_struct = protoc_struct,
        tags = tags,
    )
    if contains_cpp_src_file:
        filter_files(
            name = name + "_cc_srcs",
            srcs = [name_proto],
            extensions = ["cpp", "cc"],
            tags = tags,
        )
    filter_files(
        name = name + "_cc_hdrs",
        srcs = [name_proto],
        extensions = ["h"],
        tags = tags,
    )
    cc_library(
        name = name,
        srcs = [
            ":{}_cc_srcs".format(name),
        ] if contains_cpp_src_file else [],
        hdrs = [
            ":{}_cc_hdrs".format(name),
        ],
        includes = ["."],
        tags = tags,
        deps = kwargs.get("deps", []) + cc_protos + _isolate_service_deps,
        **{k: v for (k, v) in kwargs.items() if k != "deps"}
    )
