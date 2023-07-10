/**
 * Note: This is currently unused.
 */
#ifndef SRC_INCLUDE_JUGGLER_RPC_CTRL_H_
#define SRC_INCLUDE_JUGGLER_RPC_CTRL_H_

#include <glog/logging.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>
#include <grpc/grpc.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <thread>

using grpc::ServerContext;
using grpc::Service;
using grpc::Status;

/**
 * @brief Abstract template class for gRPC-based controller for Orion apps.
 * This can be instantiated by any application that needs to expose a control
 * plane API over gRPC. The API is discoverable with reflection.
 */
template <class T>
class OrionRpcCtrl {
 public:
  static_assert(std::is_base_of<grpc::Service, T>::value,
                "T should inherit from grpc::Service");

  /**
   * @param addr String reference to the address on which to bind the gRPC
   * server. Example format: 0.0.0.0:5000.
   * @param service shared pointer to an object that implements the API. It
   * needs to inherit from grpc::Service, so that it can be registered.
   */
  OrionRpcCtrl(const std::string &addr, T *service)
      : server_address_(addr),
        service_(CHECK_NOTNULL(service)),
        builder_(CHECK_NOTNULL(new grpc::ServerBuilder())),
        terminate_cb_(nullptr) {}
  OrionRpcCtrl(const OrionRpcCtrl &) = delete;
  OrionRpcCtrl &operator=(const OrionRpcCtrl &) = delete;

  /**
   * @brief Start the gRPC server and wait for requests. This call is blocking.
   */
  void Run() {
    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    builder_.get()->AddListeningPort(server_address_,
                                     grpc::InsecureServerCredentials());
    builder_.get()->RegisterService(service_.get());
    std::unique_ptr<grpc::Server> server(builder_.get()->BuildAndStart());
    LOG(INFO) << "OrionRpcCtrl is listening on: " << server_address_;

    // Set the termination callback.
    terminate_cb_ = [&server]() { server->Shutdown(); };

    // Block and wait for RPC requests.
    server->Wait();
  }

  /// Asynchronously terminate the server.
  void Terminate() {
    CHECK_NOTNULL(terminate_cb_);
    std::thread async_terminate([this]() { terminate_cb_(); });
    async_terminate.detach();
  }

 private:
  const std::string server_address_;
  std::shared_ptr<T> service_;
  std::unique_ptr<grpc::ServerBuilder> builder_;
  std::function<void()> terminate_cb_;
};

#endif  // SRC_INCLUDE_JUGGLER_RPC_CTRL_H_
