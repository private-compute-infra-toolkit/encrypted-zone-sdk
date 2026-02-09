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

load("@rules_rust//rust:defs.bzl", "rust_library")
load("//tools/plugin/ez_protoc:ez_plugin.bzl", "ez_plugin")
load(
    "//tools/plugin/internal:ez_sdk.bzl",
    "default_templates_dir",
    "ez_sdk_backend_protoc",
    "ez_sdk_server_bridge_protoc",
    "rs_backend_protoc",
    "rs_backend_template_plugins",
    "rs_server_bridge_protoc",
    "rs_server_bridge_template_plugins",
)

# Keep it sorted
_isolate_bridge_deps = [
    Label("//core/rust:core"),
    Label("//enforcer/v1:data_scope_rust"),
    Label("//enforcer/v1:enforcer_rust"),
    Label("@crates//:anyhow"),
    Label("@crates//:dashmap"),
    Label("@crates//:futures"),
    Label("@crates//:hyper-util"),
    Label("@crates//:log"),
    Label("@crates//:memmap2"),
    Label("@crates//:once_cell"),
    Label("@crates//:prost"),
    Label("@crates//:rand"),
    Label("@crates//:tokio"),
    Label("@crates//:tokio-stream"),
    Label("@crates//:tonic"),
    Label("@crates//:tower"),
]

# Keep it sorted
_isolate_bridge_proc_macro_deps = [
    Label("//core/rust:core_macros"),
    Label("@crates//:async-trait"),
    Label("@crates//:trait-set"),
]

def _custom_templates_proto_plugin(*, name, templates_dir, sdk_plugins):
    for plugin in sdk_plugins:
        ez_plugin(
            name = plugin.name,
            outputs = plugin.outputs,
            templates = Label(templates_dir),
            visibility = ["//visibility:public"],
        )

def ez_client_server_bridge(
        *,
        name,
        proto_basename,
        protos,
        crate_name,
        rust_protos,
        **kwargs):
    """
    Top-level macro generating Rust library target for services defined in EZ Isolate SDK proto files.

    Args:
        name: name of target, basename of ancillary targets.
        proto_basename: basename of the protobuf source file. Used as the basename of the generated header files.
        protos: list of proto_library targets
        crate_name: crate_name for generated rust_library
        rust_protos: list of rust_prost_library targets
        **kwargs: attributes for rust_library and those common to bazel build rules.

    Targets:
        <name> -- rust_library

    Returns:
        Providers:
            - EzPluginInfo
            - DefaultInfo
    """

    name_proto = name + "_proto_rs_plugin"
    protoc_struct = struct(
        protoc_rule = rs_server_bridge_protoc,
        sdk_plugins = rs_server_bridge_template_plugins,
        templates_dir = default_templates_dir,
    )

    ez_sdk_server_bridge_protoc(
        name = name_proto,
        protos = protos,
        proto_basename = proto_basename,
        protoc_struct = protoc_struct,
    )

    crate_root = "{}_lib.rs".format(proto_basename)
    copy_crate_root = "copy_{}".format(crate_root)

    # Copy crate root to a new, non-conflicting file.
    # The 'cmd' attribute uses a shell 'for' loop to find the correct file.
    native.genrule(
        name = name + "_copy_crate_root",
        srcs = [":" + name_proto],
        outs = [copy_crate_root],
        # CORRECTED COMMAND:
        cmd = """
        for f in $(locations :{}); do
          if [[ "$$f" == *"/{}" ]]; then
            cp "$$f" $(OUTS);
            exit 0;
          fi;
        done;
        echo 'ERROR: Crate root {} not found in outputs of {}' >&2;
        exit 1""".format(
            name_proto,
            crate_root,
            crate_root,
            name_proto,
        ),
    )

    rust_library(
        name = name,
        srcs = [
            name_proto,
            ":" + copy_crate_root,
        ],
        crate_name = crate_name,
        crate_root = copy_crate_root,
        edition = "2021",
        proc_macro_deps = kwargs.get("proc_macro_deps", []) + _isolate_bridge_proc_macro_deps,
        deps = kwargs.get("deps", []) + rust_protos + _isolate_bridge_deps,
        **{k: v for (k, v) in kwargs.items() if k != "deps"}
    )

def ez_isolate_service(
        *,
        name,
        proto_basename,
        protos,
        crate_name,
        rust_protos,
        crate_root = "",
        protoc_struct = struct(
            protoc_rule = rs_backend_protoc,
            sdk_plugins = rs_backend_template_plugins + rs_server_bridge_template_plugins,
            templates_dir = default_templates_dir,
        ),
        **kwargs):
    """
    Top-level macro generating Rust library target for services defined in EZ Isolate SDK proto files.

    Args:
        name: name of target, basename of ancillary targets.
        proto_basename: basename of the protobuf source file. Used as the basename of the generated header files.
        protos: list of proto_library targets
        crate_name: crate_name for generated rust_library
        rust_protos: list of rust_prost_library targets
        crate_root: crate_root for generated rust_library. Only required for custom templates
        protoc_struct: Struct returned from create_ez_protoc_rule, with three attributes:
          protoc_rule: Bazel rule used to run protoc (defaults to rs_backend_protoc)
          sdk_plugins: List of sdk plugins to generate code based on templates supplied to create_ez_protoc_rule
          templates_dir: Label to a filegroup containing custom templates. If non-empty, custom proto plugins will be generated
        **kwargs: attributes for rust_library and those common to bazel build rules.

    Generates:
        <proto_basename>_isolate_rpc_service.rs
        <proto_basename>_isolate_stub.rs

    Targets:
        <name> -- rust_library

    Returns:
        Providers:
            - EzPluginInfo
            - DefaultInfo
    """
    name_proto = name + "_proto_rs_plugin"
    if protoc_struct.templates_dir != default_templates_dir:
        if not crate_root:
            fail("crate_root is a required argument for custom templates.")
        _custom_templates_proto_plugin(
            name = name,
            templates_dir = protoc_struct.templates_dir,
            sdk_plugins = protoc_struct.sdk_plugins,
        )
    else:
        crate_root = "{}_lib.rs".format(proto_basename)

    ez_sdk_backend_protoc(
        name = name_proto,
        protos = protos,
        proto_basename = proto_basename,
        protoc_struct = protoc_struct,
    )

    copy_crate_root = "copy_{}".format(crate_root)

    # Copy crate root to a new, non-conflicting file.
    # The 'cmd' attribute uses a shell 'for' loop to find the correct file.
    native.genrule(
        name = name + "_copy_crate_root",
        srcs = [":" + name_proto],
        outs = [copy_crate_root],
        # CORRECTED COMMAND:
        cmd = """
        for f in $(locations :{}); do
          if [[ "$$f" == *"/{}" ]]; then
            cp "$$f" $(OUTS);
            exit 0;
          fi;
        done;
        echo 'ERROR: Crate root {} not found in outputs of {}' >&2;
        exit 1""".format(
            name_proto,
            crate_root,
            crate_root,
            name_proto,
        ),
    )

    rust_library(
        name = name,
        srcs = [
            name_proto,
            ":" + copy_crate_root,
        ],
        crate_name = crate_name,
        crate_root = copy_crate_root,
        edition = "2021",
        proc_macro_deps = kwargs.get("proc_macro_deps", []) + _isolate_bridge_proc_macro_deps,
        deps = kwargs.get("deps", []) + rust_protos + _isolate_bridge_deps,
        **{k: v for (k, v) in kwargs.items() if k != "deps"}
    )
