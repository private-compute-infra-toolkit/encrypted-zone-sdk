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

"""Helper rules and protoc plugin structs for the EZ Isolate SDK."""

load("@com_google_protobuf//bazel/common:proto_info.bzl", "ProtoInfo")
load(
    "//tools/plugin/ez_protoc:ez_plugin.bzl",
    "EzPluginInfo",
)
load(
    "//tools/plugin/ez_protoc:ez_protoc.bzl",
    "protoc_impl",
)

# the template_dir_name must be a valid string value for unix directories, the
# path does not need to exist -- the golang plugin and the plugin options both
# use this string value through this template_dir_name variable
template_dir_name = "ez-templates"

default_templates_dir = "//tools/plugin/tmpl"

def get_template_options(suffix, template_file):
    return "{tmpl_dir}/{tmpl},{basename}{suffix}".format(
        tmpl_dir = template_dir_name,
        basename = "{basename}",
        suffix = suffix,
        tmpl = template_file,
    )

def get_proto_compile_attrs(plugins):
    return dict(
        protos = attr.label_list(
            mandatory = True,
            providers = [ProtoInfo],
            doc = "List of labels that provide the ProtoInfo provider (such as proto_library from //third_party/protobuf)",
        ),
        template_transform = attr.string_dict(
            doc = "Information for generation of code based on templates, defined as a dict of plugin label -> template_str, where template_str is a string with the format {template_path},{generated_filename}",
        ),
        proto_basename = attr.string(
            doc = "The base name to use for generated files (without extensions)",
        ),
        _protocol_compiler = attr.label(
            default = "@com_google_protobuf//:protoc",
            cfg = "exec",
            allow_files = True,
            executable = True,
        ),
        _plugins = attr.label_list(
            providers = [EzPluginInfo],
            default = [Label("{}:{}".format(plugin.package, plugin.name)) for plugin in plugins],
        ),
    )

def _ez_sdk_protoc(*, name, protoc_rule, plugins, protos, proto_basename, **kwargs):
    protoc_rule(
        name = name,
        template_transform = {
            str(Label("{}:{}".format(p.package, p.name))): p.option.format(basename = proto_basename)
            for p in plugins
            if hasattr(p, "option")
        },
        proto_basename = proto_basename,
        protos = protos,
        **kwargs
    )

cc_backend_template_plugins = [
    struct(
        name = "ez_sdk_cc_backend_plugin{}".format(i),
        option = get_template_options(plugin.suffix, plugin.template_file),
        outputs = ["{basename}" + plugin.suffix],
        package = "//tools/plugin",
    )
    for i, plugin in enumerate([
        struct(
            template_file = "hpp_grpc_pb.tmpl",
            suffix = ".grpc.pb.h",
        ),
        struct(
            template_file = "hpp_server_reader_writer.tmpl",
            suffix = "_server_reader_writer.h",
        ),
        struct(
            template_file = "hpp_client_reader_writer.tmpl",
            suffix = "_client_reader_writer.h",
        ),
    ])
]

rs_backend_template_plugins = [
    struct(
        name = "ez_sdk_rs_backend_plugin{}".format(i),
        option = get_template_options(plugin.suffix, plugin.template_file),
        outputs = ["{basename}" + plugin.suffix],
        package = "//tools/plugin",
    )
    for i, plugin in enumerate([
        struct(
            template_file = "rs_isolate_rpc_service.tmpl",
            suffix = "_isolate_rpc_service.rs",
        ),
        struct(
            template_file = "rs_isolate_stub.tmpl",
            suffix = "_isolate_stub.rs",
        ),
    ])
]

rs_server_bridge_template_plugins = [
    struct(
        name = "ez_sdk_rs_server_bridge_plugin{}".format(i),
        option = get_template_options(plugin.suffix, plugin.template_file),
        outputs = ["{basename}" + plugin.suffix],
        package = "//tools/plugin",
    )
    for i, plugin in enumerate([
        struct(
            template_file = "rs_lib.tmpl",
            suffix = "_lib.rs",
        ),
    ])
]

cc_backend_protoc = rule(
    implementation = protoc_impl,
    attrs = get_proto_compile_attrs(cc_backend_template_plugins),
)

rs_backend_protoc = rule(
    implementation = protoc_impl,
    attrs = get_proto_compile_attrs(rs_backend_template_plugins + rs_server_bridge_template_plugins),
)

def ez_sdk_backend_protoc(*, name, protos, proto_basename, protoc_struct, **kwargs):
    _ez_sdk_protoc(
        name = name,
        protoc_rule = protoc_struct.protoc_rule,
        plugins = protoc_struct.sdk_plugins,
        protos = protos,
        proto_basename = proto_basename,
        **kwargs
    )

def get_all_ez_sdk_plugins():
    return cc_backend_template_plugins + rs_backend_template_plugins

rs_server_bridge_protoc = rule(
    implementation = protoc_impl,
    attrs = get_proto_compile_attrs(rs_server_bridge_template_plugins),
)

def ez_sdk_server_bridge_protoc(*, name, protos, proto_basename, protoc_struct, **kwargs):
    _ez_sdk_protoc(
        name = name,
        protoc_rule = protoc_struct.protoc_rule,
        plugins = protoc_struct.sdk_plugins,
        protos = protos,
        proto_basename = proto_basename,
        **kwargs
    )

def get_all_ez_server_bridge_plugins():
    return rs_server_bridge_template_plugins

