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

"""EZ SDK plugin rules and providers."""

EzPluginInfo = provider(
    doc = "Information about the behaviour and location of a protoc plugin",
    fields = {
        "label": "The plugin label",
        "name": "The plugin name",
        "outputs": "Per-proto output filenames, templated like '{basename}_custom.h'",
        "templates": "The label to the templates filegroup.",
        "tool": "The plugin binary",
        "tool_provider": "The plugin DefaultInfo.",
    },
)

def _ez_plugin_impl(ctx):
    return [
        EzPluginInfo(
            name = ctx.attr.name,
            label = ctx.label,
            tool = ctx.attr.tool,
            tool_provider = ctx.attr.tool[DefaultInfo],
            outputs = ctx.attr.outputs,
            templates = ctx.files.templates,
        ),
        DefaultInfo(
            files = depset(ctx.files.templates),
        ),
    ]

ez_plugin = rule(
    implementation = _ez_plugin_impl,
    attrs = {
        "outputs": attr.string_list(
            doc = "Per-proto output filenames, templated like '{basename}_custom.h', where {basename} is replaced with the proto basename.",
        ),
        "templates": attr.label(
            doc = "The label to the templates filegroup.",
            allow_files = True,
        ),
        "tool": attr.label(
            doc = "The label of plugin binary target.",
            default = Label("//tools/plugin:ez_sdk_plugin"),
            cfg = "exec",
            allow_files = True,
            executable = True,
        ),
    },
)
