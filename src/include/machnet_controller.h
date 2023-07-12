/**
 * @file nsaas_controller.h
 * @brief This file contains the NSaaSController class. It is responsible for
 * the control plane of the NSaaS stack.
 */
#ifndef SRC_INCLUDE_NSAAS_CONTROLLER_H_
#define SRC_INCLUDE_NSAAS_CONTROLLER_H_

#include <channel.h>
#include <nsaas_config.h>
#include <nsaas_ctrl.h>
#include <nsaas_engine.h>
#include <ud_socket.h>
#include <uuid/uuid.h>

#include <csignal>
#include <thread>

#include "common.h"

namespace juggler {
/**
 * @class NSaaSController
 * @brief This class is responsible for the control plane of the NSaaS stack.
 * It is responsible for creating new channels, listening to specific ports, and
 * for creating new connections based on the requests received from the
 * applications.
 */
class NSaaSController {
 public:
  using UDSocket = juggler::net::UDSocket;
  using UDServer = juggler::net::UDServer;
  using ChannelManager = juggler::shm::ChannelManager<juggler::shm::Channel>;
  // Timeout for idle connections in seconds.
  static constexpr uint32_t kConnectionTimeoutInSec = 2;
  NSaaSController(const NSaaSController &) = delete;
  // Delete constructor and assignment operator.
  NSaaSController &operator=(const NSaaSController &) = delete;

  // Create a singleton instance of the controller.
  static NSaaSController *Create(const std::string &conf_file) {
    if (instance_ == nullptr) {
      instance_ = new NSaaSController(conf_file);
    }
    return instance_;
  }

  static void ReleaseInstance() {
    if (instance_ != nullptr) {
      delete instance_;
      instance_ = nullptr;
    }
  }

  static void sig_handler(int signum) {
    if (signum == SIGINT && instance_ != nullptr) {
      LOG(INFO) << "Received SIGINT. Stopping the controller.";
      instance_->Stop();
    }
  }

  /**
   * @brief Start the controller.
   * It spawns a new thread that listens for incoming connections, and handles
   * requests.
   */
  void Run();
  /**
   * @brief Check if the controller is running.
   * @return True if the controller is running, false otherwise.
   */
  bool IsRunning() const { return running_; }
  /**
   * @brief Stop the controller.
   * It closes the listening socket, and sleeps for a few seconds to allow the
   * controller thread to terminate.
   */
  void Stop();

 private:
  // Default constructor is private.
  explicit NSaaSController(const std::string &conf_file);
  /**
   * @brief Callback to handle new connections to the controller.
   *
   * @param s The socket that is being connected.
   */
  bool HandleNewConnection(UDSocket *s);

  /**
   * @brief Callback to handle new messages to the controller.
   * This function is responsible for executing the core logic of the
   * controller.
   * Signature is defined by libusockets.
   * @param s The socket that is being connected.
   * @param data The data received.
   * @param length The length of the data received.
   */
  void HandleNewMessage(UDSocket *s, const char *data, size_t length);

  /**
   * @brief Callback to handle passive close of the socket.
   * Signature is defined by libusockets.
   * @param s The socket that is being closed.
   */
  void HandlePassiveClose(UDSocket *s);

  /**
   * @brief Callback to handle connection timeout.
   * Signature is defined by libusockets.
   * @param s The socket that timed out.
   * @attention This function is not implemented yet.
   */
  void HandleTimeout(UDSocket *s);

  /**
   * @brief Callback to handle shutdown of a client.
   * Signature is defined by libusockets.
   * @param s The socket that is being closed.
   * @param code The code of the shutdown.
   * @param reason The reason for the shutdown.
   */
  void HandleShutdown(UDSocket *s, int code, void *reason);

  /**
   * @brief Register a new application with the controller.
   * @param[in] app_uuid UUID of the originating application.
   * @param[in] app_info Information about the application to be registered.
   * @return True if the channel has been registered successfully, false
   * otherwise.
   */
  bool RegisterApplication(const uuid_t app_uuid,
                           const nsaas_app_info_t *app_info);

  /**
   * @brief Unregister an application from the controller. Releases all the
   * resources associated with this application.
   * @param[in] app_uuid UUID of the originating application.
   */
  void UnregisterApplication(const uuid_t app_uuid);

  /**
   * @brief Create a new channel.
   * @param[in] app_uuid     UUID of the originating application.
   * @param[in] channel_info Information about the channel to be created.
   * @param[out] fd         The file descriptor of the channel (-1 on failure).
   * @return True if the channel has been created successfully, false otherwise.
   */
  bool CreateChannel(const uuid_t app_uuid,
                     const nsaas_channel_info_t *channel_info, int *fd);

  /**
   * @brief The main loop of the controller.
   */
  void RunController();

  /**
   * @brief Stop the controller. (thread-safe)
   */
  void StopController();

 private:
  static inline NSaaSController *instance_;
  NSaaSConfigProcessor config_processor_;
  ChannelManager channel_manager_;
  bool running_{false};
  dpdk::Dpdk dpdk_{};
  std::vector<std::shared_ptr<dpdk::PmdPort>> pmd_ports_{};
  std::vector<std::shared_ptr<NSaaSEngine>> engines_{};
  std::unique_ptr<UDServer> server_{nullptr};
  std::unordered_map<std::string, std::unordered_set<std::string>>
      applications_registered_{};
};
}  // namespace juggler

#endif  //  SRC_INCLUDE_NSAAS_CONTROLLER_H_
