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

"""A Starlark rule to filter the default output files of rule into a set matching one of specified extensions."""

def _filter_files_impl(ctx):
    files = depset([
        f
        for f in ctx.files.srcs
        if f.extension in ctx.attr.extensions
    ])
    return [DefaultInfo(files = files)]

filter_files = rule(
    implementation = _filter_files_impl,
    attrs = {
        "extensions": attr.string_list(
            doc = "The list of file extensions to include, leading dot omitted.",
        ),
        "srcs": attr.label_list(
            allow_files = True,
            doc = "Label of the targets to with output files to be filtered.",
        ),
    },
)
