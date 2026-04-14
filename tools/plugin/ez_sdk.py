# !/usr/bin/env python3
#
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

r"""A protoc plugin for generating code using Jinja2 templates.

This plugin reads a CodeGeneratorRequest from stdin, renders a Jinja2 template
with the proto descriptors, and writes a CodeGeneratorResponse to stdout.

The plugin expects parameters to be passed via the `--plugin_opt` flag of protoc,
in a comma-separated key=value format. The following parameters are supported:

  - template_dir: The directory containing the Jinja2 templates.
      Defaults to the value of the TEMPLATE_PATH environment variable, or "."
      if not set.
  - template_name: The name of the Jinja2 template file within `template_dir`
      to be used for code generation. This parameter is REQUIRED.
  - output_name: The desired filename for the generated output file.
      Defaults to "output.rs".

Example protoc usage:
  protoc --plugin=protoc-gen-ez_sdk=/path/to/ez_sdk.py \
    --ez_sdk_out=./output_dir \
    --ez_sdk_opt=template_dir=/path/to/templates,template_name=my_svc.j2,output_name=my_service.rs \
    my_service.proto
"""

import logging
import os
import re
import sys
import traceback

import jinja2
from jinja2 import exceptions

from google.protobuf.compiler import plugin_pb2
from google.protobuf import descriptor_pb2

TemplateError = exceptions.TemplateError
FileSystemLoader = jinja2.FileSystemLoader
Environment = jinja2.Environment

# Configure logging to stderr so it doesn't corrupt stdout
logging.basicConfig(stream=sys.stderr, level=logging.INFO)

_LOWER_UPPER_RE = re.compile(r'([a-z0-9])([A-Z])')

_UPPER_UPPER_RE = re.compile(r'([A-Z])([A-Z][a-z0-9])')


def _snakecase(s: str) -> str:
  # Insert '_' between a lowercase/digit and an uppercase letter.
  s1 = _LOWER_UPPER_RE.sub(r'\1_\2', s)
  # Insert '_' between two consecutive uppercase letters followed
  # by a lowercase/digit.
  s2 = _UPPER_UPPER_RE.sub(r'\1_\2', s1)
  return s2.lower()


def _upper(s: str) -> str:
  return s.upper()


def _ext(value: str) -> str:
  _, ext = os.path.splitext(value)
  return ext


def _trim_suffix(value: str, suffix: str) -> str:
  if suffix and value.endswith(suffix):
    return value[: -len(suffix)]
  return value


def _lstrip(value: str, prefix: str) -> str:
  if prefix and value.startswith(prefix):
    return value[len(prefix) :]
  return value


def _split(value: str, separator: str) -> list[str]:
  return value.split(separator)


def create_jinja_environment(template_dir: str) -> Environment:
  """Creates and configures a Jinja2 Environment."""
  env = Environment(
      loader=FileSystemLoader(template_dir), extensions=['jinja2.ext.do']
  )
  env.filters['snakecase'] = _snakecase
  env.filters['upper'] = _upper
  env.filters['ext'] = _ext
  env.filters['trim_suffix'] = _trim_suffix
  env.filters['lstrip'] = _lstrip
  env.filters['split'] = _split
  return env


def generate_code(
    request: plugin_pb2.CodeGeneratorRequest, env: Environment | None = None
) -> plugin_pb2.CodeGeneratorResponse:
  """Generates code based on the CodeGeneratorRequest using Jinja2.

  Args:
    request: The CodeGeneratorRequest proto.
    env: An optional jinja2.Environment. If not provided, one will be created.

  Returns:
    A CodeGeneratorResponse proto.
  """
  response = plugin_pb2.CodeGeneratorResponse(
      supported_features=plugin_pb2.CodeGeneratorResponse.FEATURE_PROTO3_OPTIONAL
      | plugin_pb2.CodeGeneratorResponse.FEATURE_SUPPORTS_EDITIONS,
      minimum_edition=descriptor_pb2.EDITION_2023,
      maximum_edition=descriptor_pb2.EDITION_2024,
  )

  # NOTE: This parameter parsing assumes values do not contain commas.
  # If comma-separated values are needed within a single parameter,
  # a more robust parsing mechanism (e.g., with escaping) would be required.
  params = dict(
      p.split('=', 1) for p in request.parameter.split(',') if '=' in p
  )

  template_dir = params.get(
      'template_dir', os.environ.get('TEMPLATE_PATH', '.')
  )
  template_name = params.get('template_name')

  if not template_name:
    response.error = 'Missing parameter: template_name'
    return response

  if env is None:
    env = create_jinja_environment(template_dir)

  try:
    template = env.get_template(template_name)
  except TemplateError as e:
    error_msg = ''.join(traceback.format_exception_only(type(e), e)).strip()
    response.error = f'Failed to load template {template_name}: {error_msg}'
    return response

  services_param = params.get('services')
  registry_services = (
      set(services_param.split(';')) if services_param else set()
  )

  def _get_fully_qualified_name(proto_file, svc):
    if proto_file.package:
      return f'{proto_file.package}.{svc.name}'
    return svc.name

  # Avoid O(n*m) complexity by converting the list to a set for lookup.
  files_to_generate = set(request.file_to_generate)
  ez_services_map = {}
  for proto_file in request.proto_file:
    ez_services_map[proto_file.name] = [
        svc
        for svc in proto_file.service
        if _get_fully_qualified_name(proto_file, svc) in registry_services
    ]

  context = {
      'Files': [f for f in request.proto_file if f.name in files_to_generate],
      'Request': request,
      'EzServices': ez_services_map,
  }

  try:
    output_content = template.render(context)
    output_filename = params.get('output_name', 'output.rs')

    response.file.add(name=output_filename, content=output_content)

  except TemplateError as e:
    error_msg = ''.join(traceback.format_exception_only(type(e), e)).strip()
    response.error = f'Failed to render template {template_name}: {error_msg}'
    return response

  return response


def main() -> None:
  with sys.stdin.buffer as f:
    data = f.read()
  request = plugin_pb2.CodeGeneratorRequest.FromString(data)
  response = generate_code(request)
  with sys.stdout.buffer as f:
    f.write(response.SerializeToString())


if __name__ == '__main__':
  main()
