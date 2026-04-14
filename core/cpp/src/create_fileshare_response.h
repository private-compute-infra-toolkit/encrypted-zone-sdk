/*
 * Copyright 2026 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CORE_CPP_SRC_CREATE_FILESHARE_RESPONSE_H
#define CORE_CPP_SRC_CREATE_FILESHARE_RESPONSE_H

#include <grpcpp/support/status.h>

#include <string>

namespace IsolateEzBridgeSdk {

// Result from file sharing. Shared file handle will be populated if
// the file sharing succeeded, otherwise a suitable status will be
// set.
struct CreateFileshareResponse {
  // TODO: Remove status field.
  grpc::Status status;
  std::string fileshare_handle;
  std::string shared_path;
  std::string staging_path;
};

}  // namespace IsolateEzBridgeSdk

#endif  // CORE_CPP_SRC_CREATE_FILESHARE_RESPONSE_H
