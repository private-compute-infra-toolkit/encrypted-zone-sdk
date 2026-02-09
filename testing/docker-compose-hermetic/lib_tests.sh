#!/bin/bash
# Copyright 2025 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

#
# helper functions for local testing
#

# require this script to be sourced rather than executed
if ! (return 0 2>/dev/null); then
  printf "Error: Script %s must be sourced\n" "${BASH_SOURCE[0]}" &>/dev/stderr
  exit 1
fi

#
# init() -- initialize the test script
#
# sets these global variables:
#   _DOCKER_NETWORK
#   SUT_PATH
# sets and exports these global variables:
#   WORKSPACE
#
function ez_testing::init() {
  declare -r script_dir="$1"
  if ! [[ -v WORKSPACE ]]; then
    local -r ws="$(git rev-parse --show-toplevel)"
    declare -g -r -x WORKSPACE="${ws}"
  fi
  declare -g -r -x TOP_LEVEL_DIR="${WORKSPACE}"
  declare -g -r -x NODE_WORKSPACE="${TOP_LEVEL_DIR}/node"
  declare -g -r -x SDK_WORKSPACE="${TOP_LEVEL_DIR}"
  declare -g -r -x DEVKIT="${TOP_LEVEL_DIR}/devkit"
  local -r sut_path="$(realpath --relative-to="${SDK_WORKSPACE}" "${script_dir}")"
  # shellcheck disable=SC2034
  declare -g -r SUT_PATH="${sut_path}"

  declare -g -r _DOCKER_NETWORK=ez-local-functest
}

function ez_testing::ensure_docker_compose() {
  # Check for version 2 or higher (e.g., 2.x, 5.x...)
  if ! docker compose version --short | grep -E -q '^([2-9]|[1-9][0-9])\.' &>/dev/null; then
    printf "docker compose v2+ is required\n" &>/dev/stderr
    exit 1
  fi
}

function ez_testing::docker_compose_cleanup() {
  declare -r -i status=$?
  if [[ $# -gt 0 ]]; then
    docker compose "${@}" down || true
  fi
  return ${status}
}

function ez_testing::build_functionaltest_cli() {
  declare -g -r FUNCTIONALTEST_CLI_IMAGE="$1"
  # shellcheck disable=SC2086
  docker buildx build --progress=plain ${EXTRA_DOCKER_BUILDX_ARGS} \
    --tag="${FUNCTIONALTEST_CLI_IMAGE}" \
    "${SDK_WORKSPACE}"/functionaltest-system
}

function ez_testing::load_enforcer_server() {
  declare -n image_tag_="$1"

  cd "${SDK_WORKSPACE}" || exit 1
  image_tag_="$(docker load -i "${ENFORCER_TAR_PATH}" | awk '/^Loaded image: / {print $3}')"
  if [[ -z ${image_tag_} ]]; then
    printf "Could not determine image tag from docker load\n" >&2
    exit 1
  fi
}

# Copies associated manifest_json & server_bundle_tar to the SUT_PATH.
function ez_testing::build_isolate() {
  cd "${SDK_WORKSPACE}" || exit 1
  declare -r server_bundle_target="$1"; shift

  # Split server_bundle_target into server_dir and server_bundle_ based on colon
  server_dir="${server_bundle_target%:*}"
  server_bundle_name_="${server_bundle_target#*:}"
  escaped_server_dir=${server_dir//\//_}

  "${DEVKIT}"/build bazel build "//${server_bundle_target}"
  cp -n "${SDK_WORKSPACE}/bazel-bin/${server_dir}/${server_bundle_name_}.tar" "${SDK_WORKSPACE}/${SUT_PATH}/${escaped_server_dir}_${server_bundle_name_}.tar"
}

function ez_testing::check_env_exists() {
  local -r env_var_name="$1"
  declare -n _var=$1
  local -r default_val="$2"
  if [[ -n ${_var} ]]; then
    return
  fi
  if [[ -z ${default_val} ]]; then
    if ! [[ -v ${env_var_name} ]]; then
      printf "env var [%s] is not set, no default specified\n" "${env_var_name}" &>/dev/stderr
      exit 1
    fi
  else
    _var="${default_val}"
  fi
}
