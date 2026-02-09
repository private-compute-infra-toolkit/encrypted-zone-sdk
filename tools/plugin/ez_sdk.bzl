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

load(
    "//tools/plugin:cc_ez_sdk.bzl",
    _ez_isolate_service_cc = "ez_isolate_service",
)
load(
    "//tools/plugin:rs_ez_sdk.bzl",
    _ez_client_server_bridge_rs = "ez_client_server_bridge",
    _ez_isolate_service_rs = "ez_isolate_service",
)
load("//tools/plugin/ez_protoc:ez_protoc.bzl", "protoc_impl")
load(
    "//tools/plugin/internal:ez_sdk.bzl",
    "get_proto_compile_attrs",
    "get_template_options",
)

ez_isolate_service_cc = _ez_isolate_service_cc
ez_isolate_service_rs = _ez_isolate_service_rs
ez_client_server_bridge_rs = _ez_client_server_bridge_rs

def _get_sdk_plugins(service_name, service_package, template_structs, lang):
    """
    Generates a list of plugin configuration structs for the SDK.

    Args:
      service_name (str): The name of the service for which plugins are being generated.
      service_package (str): The Bazel package containing the service plugins.
      template_structs (list): A list of template plugin objects, each containing
        'suffix' and 'template_file' attributes.
      lang (str): The programming language for which the plugins are being generated (e.g., "cc", "rs").

    Returns:
      list: A list of struct objects, each representing a plugin configuration with the following fields:
        - name (str): Unique name for the plugin.
        - option: Options generated from the plugin's suffix and template file.
        - outputs (list): List of output file names for the plugin.
        - package (str): The Bazel package name.
    """
    return [
        struct(
            name = "{}_ez_sdk_{}_backend_plugin{}".format(service_name, lang, i),
            option = get_template_options(plugin.suffix, plugin.template_file),
            outputs = ["{basename}" + plugin.suffix],
            package = service_package,
        )
        for i, plugin in enumerate(template_structs)
    ]

def create_ez_protoc_rule(service_name, service_package, template_structs, templates_dir, lang = "cc"):
    """Creates a protoc rule configured with the specified sdk_plugins and service_package.

    Args:
        service_name: Name of the service.
        service_package: Package path where the plugins are defined
        template_structs: List of (template_file, suffix) structs to be generated
        templates_dir: Label to a filegroup containing custom templates.
        lang: The language for which the protoc rule is being created (e.g., "cc", "rs").

    Returns:
        A struct containing a bazel rule configured with protoc_impl and the list of sdk plugins based on template_structs.
    """
    sdk_plugins = _get_sdk_plugins(service_name, service_package, template_structs, lang)

    return struct(
        sdk_plugins = sdk_plugins,
        protoc_rule = rule(
            implementation = protoc_impl,
            attrs = get_proto_compile_attrs(sdk_plugins),
        ),
        templates_dir = templates_dir,
    )
