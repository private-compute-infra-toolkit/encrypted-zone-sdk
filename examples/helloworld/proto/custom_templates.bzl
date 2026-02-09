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

"""Custom templates for the HelloWorld service."""

load("//tools/plugin:ez_sdk.bzl", "create_ez_protoc_rule")

# Should be the name passed into ez_isolate_service_cc
service_name = "custom_greeter_service"

# should be the package where ez_isolate_service_cc is defined
service_package = "//examples/helloworld/proto"

# Should be a label to a filegroup containing custom templates, e.g., "//path/to/my/templates:all_files"
templates_dir = "//examples/helloworld/proto/custom_templates"

template_structs = [
    struct(
        template_file = "hpp_custom.tmpl",
        suffix = "_custom_tmpl.h",
    ),
    struct(
        template_file = "cpp_custom.tmpl",
        suffix = "_custom_tmpl.cpp",
    ),
]

custom_backend_struct = create_ez_protoc_rule(service_name, service_package, template_structs, templates_dir)
custom_backend_protoc = custom_backend_struct.protoc_rule
