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

#ifndef ISOLATE_EZ_BRIDGE_CLIENT_H
#define ISOLATE_EZ_BRIDGE_CLIENT_H

#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "core/cpp/src/mem_share_response.h"
#include "enforcer/v1/isolate_ez_bridge.pb.h"
#include "enforcer/v1/isolate_state.pb.h"

namespace grpc {
// Declaration for grpc::Status ostream operator
std::ostream& operator<<(std::ostream& os, const grpc::Status& status);
}  // namespace grpc

namespace IsolateEzBridgeSdk {

// Define a vector-like wrapper for raw memory, since std::vector wants to be
// free to resize itself and move the memory to other locations.
// Does not support STL container-style features, and does not free the raw
// memory when deleted.
template <typename T>
class Vec {
 public:
  explicit Vec(T* data, size_t size) : data_(data), size_(size) {}
  Vec() = default;

  T& operator[](size_t i) { return data_[i]; }
  const T& operator[](size_t i) const { return data_[i]; }

  size_t size() { return size_; }
  T* data() { return data_; }

 private:
  T* data_ = nullptr;
  size_t size_ = 0;
};

using enforcer::v1::IsolateState;

class IsolateEzBridgeClient {
 public:
  virtual ~IsolateEzBridgeClient() = default;
  // Instances can only be obtained via this method.
  // Currently returns a global singleton, but this is not guaranteed in future
  // implementations (e.g. thread-local may become supported.)
  static IsolateEzBridgeClient& GetInstance();
  static void SetUdsAddress(std::string path);
  static void SetReadySignalPath(std::string path);
  // For testing only. If set, GetInstance() will return this instance instead
  // of default singleton.
  static void SetInstanceForTesting(IsolateEzBridgeClient* instance);

  absl::Status ProcessReceivedSharedMemoryHandle(
      const std::string& shared_memory_handle);

  // Registers a handler for a service that is running locally in the same
  // isolate.
  // This function should only be called before IsolateRpcCall on RPC requests
  // going to the isolate itself through IsolateRpcCall. Typically this won't be
  // a problem if the server's main function starts executing queries after RPC
  // server has started. The only case it may cause problem is int main() {
  //    ...std::thread t([](){ create and invoke stub to isolate itself... })
  //    ez_stubby_server.AddIsolateRpcService(service);
  //    ez_stubby_server.StartIsolateRpcServer(absl::GetFlag(FLAGS_socket_path));
  // }
  // This should be rare and incorrect usage of RPC server.
  virtual void RegisterLocalHandler(
      const std::string& service_name,
      std::function<grpc::Status(const std::string& method_name,
                                 const std::string& request,
                                 std::string& response)>
          handler) = 0;

  // Calls another Isolate.
  // This function should only be called after all EZ server setup has finished.
  // TODO: Support empty domain == current isolate's domain as default.
  virtual grpc::Status IsolateRpcCall(grpc::ClientContext* context,
                                      const std::string& operator_domain,
                                      const std::string& service_name,
                                      const std::string& method_name,
                                      const std::string& ez_instance_id,
                                      const std::string& request_bytes,
                                      std::string& response_bytes) = 0;

  virtual void AsyncIsolateRpcCall(
      grpc::ClientContext* context, const std::string& operator_domain,
      const std::string& service_name, const std::string& method_name,
      const std::string& ez_instance_id, const std::string& request_bytes,
      std::string* response_bytes,
      absl::AnyInvocable<void(grpc::Status) &&> callback) = 0;

  virtual std::unique_ptr<::grpc::ClientReaderWriter<
      ::enforcer::v1::InvokeEzRequest, ::enforcer::v1::InvokeEzResponse>>
  IsolateStreamRpcCall(grpc::ClientContext* context,
                       const std::string& operator_domain,
                       const std::string& service_name,
                       const std::string& method_name,
                       const std::string& ez_instance_id) = 0;

  virtual bool NewIsolateState(IsolateState new_isolate_state) = 0;

  // Creates a writable shared memory region with room for a specified number
  // of items (num_items) of the requested type.
  template <typename T>
  MemShareResponse CreateMemShare(int64_t num_items, Vec<T>& shared_memory) {
    int64_t size_in_bytes = num_items * sizeof(T);
    char* ptr;
    auto mem_share_response = CreateMemShareInternal(size_in_bytes, &ptr);
    shared_memory = Vec<T>((T*)ptr, num_items);
    return mem_share_response;
  }

  // TODO: Consider wiring this more neatly into IsolateRpcCall
  // Returns the most recently received shared memory handle, or
  // an empty vector if none is pending.
  template <typename T>
  Vec<T> ReceiveMemShare() {
    Vec<char> vec = ReceiveMemShareInternal();
    uint64_t num_items = vec.size() / sizeof(T);
    int remainder = vec.size() % sizeof(T);
    if (remainder == 0) {
      return Vec<T>((T*)vec.data(), num_items);
    }
    std::cout << "ReceiveMemshare: Incompatible type\n";
    return Vec<T>();
  }

  IsolateEzBridgeClient(IsolateEzBridgeClient const&) = delete;  // No copying
  void operator=(IsolateEzBridgeClient const&) = delete;  // No assignment

 protected:
  IsolateEzBridgeClient() {}  // Protected constructor

  virtual MemShareResponse CreateMemShareInternal(int64_t num_items,
                                                  char** shared_memory) = 0;
  virtual Vec<char> ReceiveMemShareInternal() = 0;
};

}  // namespace IsolateEzBridgeSdk

#endif  // ISOLATE_EZ_BRIDGE_CLIENT_H
