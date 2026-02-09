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

"""protoc implementation for EZ SDK."""

load("@com_google_protobuf//bazel/common:proto_info.bzl", "ProtoInfo")
load("//tools/plugin/ez_protoc:ez_plugin.bzl", "EzPluginInfo")

def _copy_file(ctx, src, dest):
    """
    Copy file from src to dest.

    Adapted from https://github.com/rules-proto-grpc/rules_proto_grpc

    Args:
        ctx: the <ctx> object
        src: the source file <File>
        dest: the destination path of the file

    Returns:
        <Generated File> for the copied file

    """
    dest = ctx.actions.declare_file(dest)
    ctx.actions.run_shell(
        mnemonic = "CopyFileAndReplaceBasename",
        inputs = [src],
        outputs = [dest],
        command = "cp '{}' '{}' && sed -i 's/PROTO_BASENAME/{}/g' '{}'".format(src.path, dest.path, ctx.attr.proto_basename, dest.path),
        progress_message = "copying file {} to {}".format(src.path, dest.path),
    )
    return dest

def _descriptor_proto_path(proto, proto_info):
    """
    Convert a proto File to the path within the descriptor file.

    Adapted from https://github.com/bazelbuild/rules_go

    Args:
        proto: The proto file.
        proto_info: The ProtoInfo provider the file is from.

    Returns:
        The path to the proto file, as seen when making the descriptor.

    """
    return _strip_prefixes(proto.path, [proto_info.proto_source_root, proto.root.path, proto.owner.workspace_root])

def _strip_prefixes(path, prefixes):
    """
    Strip a prefix from a path if it exists and any remaining prefix slashes.

    Adapted from https://github.com/rules-proto-grpc/rules_proto_grpc

    Args:
        path: The path string to strip.
        prefixes: The list of prefixex to remove.

    Returns:
        The stripped path. If the prefixes is not present, this will be the same as the input.

    """
    for prefix in prefixes:
        path = path.removeprefix(prefix).removeprefix("/")
    return path

def _get_package_root(ctx):
    """
    Get the full path to the package output directory, relative to workspace root.

    Adapted from https://github.com/rules-proto-grpc/rules_proto_grpc
    Args:
        ctx: The Bazel rule execution context object.

    Returns:
        The full path.

    """
    package_root = ctx.bin_dir.path
    if ctx.label.workspace_root:
        package_root += "/" + ctx.label.workspace_root
    if ctx.label.package:
        package_root += "/" + ctx.label.package

    return package_root

def _build_protoc_args(
        ctx,
        plugin,
        descriptor_sets,
        root):
    """
    Builds list of args for invoking protoc for a given plugin.

    Args:
        ctx: The Bazel rule execution context object.
        plugin: The EzPluginInfo for the plugin to use.
        descriptor_sets: The list of descriptor sets to use.
        root: The root dir of the output files.

    Returns:
        - The list of args.
        - The inputs required for the command.

    """
    path_separator = ctx.configuration.host_path_separator
    tool_exec_path = plugin.tool_provider.files_to_run.executable.path
    template_str = ctx.attr.template_transform.get(str(plugin.label), "")

    args_list = [
        "--descriptor_set_in={}".format(path_separator.join([f.path for f in descriptor_sets])),
        "--plugin=protoc-gen-{}={}".format(plugin.name, tool_exec_path),
        "--{}_out={}:{}".format(plugin.name, template_str, root),
    ]
    return args_list

def _run_protoc_for_plugin(ctx, plugin, abs_gen_dir):
    """
    Generates files for a single plugin.

    Args:
        ctx: The Bazel rule execution context object.
        plugin: The EzPluginInfo for the plugin.
        abs_gen_dir: The absolute path to the generation directory.

    Returns:
        A list of generated File objects.
    """
    proto_infos = [dep[ProtoInfo] for dep in ctx.attr.protos]
    protoc = ctx.executable._protocol_compiler

    outputs = []
    proto_paths = []
    descriptor_sets = []
    proto_basename = ctx.attr.proto_basename
    for proto_info in proto_infos:
        descriptor_sets.extend(proto_info.transitive_descriptor_sets.to_list())

        for proto in proto_info.direct_sources:
            outputs.extend([
                ctx.actions.declare_file("{}/{}".format(
                    abs_gen_dir.rpartition("/")[-1],  # relative name of dir
                    pattern.replace("{basename}", proto_basename),  # remove .proto extension
                ))
                for pattern in plugin.outputs
            ])
            proto_paths.append(_descriptor_proto_path(proto, proto_info))

    # Remove duplicates
    outputs = depset(outputs).to_list()
    proto_paths = depset(proto_paths).to_list()
    descriptor_sets = depset(descriptor_sets).to_list()

    args_list = _build_protoc_args(
        ctx,
        plugin,
        descriptor_sets,
        abs_gen_dir,
    )
    args = ctx.actions.args()
    args.set_param_file_format("multiline")
    args.use_param_file("@%s", use_always = False)
    args.add_all(args_list)
    args.add_all(proto_paths)

    # Get the template directory from the first template file.
    if len(plugin.templates) == 0:
        fail("No templates found for plugin: {}".format(plugin.name))
    if len(depset([template.dirname for template in plugin.templates]).to_list()) != 1:
        fail("Templates for plugin {} must all be in the same directory.".format(plugin.name))
    template_dir = plugin.templates[0].dirname

    ctx.actions.run_shell(
        mnemonic = "EzProtoc",
        command = 'mkdir -p "{}" && {} "$@"'.format(abs_gen_dir, protoc.path),
        arguments = [args],
        inputs = descriptor_sets + plugin.templates,
        tools = [protoc, plugin.tool_provider.files_to_run],
        outputs = outputs,
        use_default_shell_env = True,
        env = {
            "TEMPLATE_PATH": template_dir,
        },
        progress_message = "Run protoc for {} plugin on target {}".format(
            plugin.name,
            ctx.label,
        ),
    )
    return outputs

def protoc_impl(ctx):
    """
    Rule implementation for protoc.

    Args:
        ctx: The Bazel rule execution context object.

    Returns:
        Providers:
            - DefaultInfo

    """
    plugins = [plugin[EzPluginInfo] for plugin in ctx.attr._plugins]

    # Make a temporary directory to hold the generated files.
    abs_gen_dir = "{}/_temp_{}".format(_get_package_root(ctx), ctx.label.name)

    gen_files = []
    for plugin in plugins:
        gen_files_for_plugin = _run_protoc_for_plugin(
            ctx,
            plugin,
            abs_gen_dir,
        )
        gen_files.extend(gen_files_for_plugin)

    output_files = depset([
        _copy_file(
            ctx,
            file,
            dest = _strip_prefixes(file.path, [abs_gen_dir]),
        )
        for file in gen_files
    ])

    return [
        DefaultInfo(
            files = output_files,
            runfiles = ctx.runfiles(transitive_files = output_files),
        ),
    ]
