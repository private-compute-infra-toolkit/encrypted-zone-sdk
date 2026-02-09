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

#ifndef MEM_SHARE_RESPONSE_H
#define MEM_SHARE_RESPONSE_H

#include <string>

#include <grpcpp/grpcpp.h>

// Result from memory sharing. Shared memory handle will be populated if
// the memory sharing/mapping succeeded, otherwise a suitable status will be
// set.
struct MemShareResponse {
  grpc::Status status;
  std::string shared_memory_handle;
};

#endif  // MEM_SHARE_RESPONSE_H
