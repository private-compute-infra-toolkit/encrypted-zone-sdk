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

set -o errexit

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <report_file>" >&2
    exit 1
fi

report_file="$1"

# Check if file exists
if ! [[ -f ${report_file} ]]; then
    echo "Error: Report file '$report_file' does not exist" >&2
    exit 1
fi

# Check if file is not empty
if ! [[ -f ${report_file} ]]; then
    echo "Error: Report file '$report_file' is empty" >&2
    exit 1
fi

echo "GHZ report validation passed: $report_file" >&2
