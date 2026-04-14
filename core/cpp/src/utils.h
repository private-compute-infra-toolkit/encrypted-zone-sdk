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

#ifndef CORE_CPP_SRC_UTILS_H
#define CORE_CPP_SRC_UTILS_H

#include <grpcpp/support/status.h>

#include "absl/status/status.h"

#ifndef EXPECT_OK
#define EXPECT_OK(expression) EXPECT_TRUE((expression).ok())
#endif
#ifndef ASSERT_OK
#define ASSERT_OK(expression) ASSERT_TRUE((expression).ok())
#endif

namespace IsolateEzBridgeSdk {

absl::Status ToAbslStatus(const grpc::Status& grpc_status) {
  if (grpc_status.ok()) {
    return absl::OkStatus();
  }

  // Both gRPC and Abseil status codes share the same underlying integer
  // values, so a direct static_cast is perfectly safe here.
  return absl::Status(static_cast<absl::StatusCode>(grpc_status.error_code()),
                      grpc_status.error_message());
}

grpc::Status ToGrpcStatus(const absl::Status& status) {
  if (status.ok()) return grpc::Status::OK;
  return grpc::Status(static_cast<grpc::StatusCode>(status.code()),
                      std::string(status.message()));
}

}  // namespace IsolateEzBridgeSdk

#endif  // CORE_CPP_SRC_UTILS_H

