/**
 * @file ud_socket.h
 * @brief This file contains functionality to support a server that uses Unix
 * domain sockets.
 */
#ifndef SRC_INCLUDE_UD_SOCKET_H_
#define SRC_INCLUDE_UD_SOCKET_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace juggler {
namespace net {

// Forward declaration.
class UDServer;

class UDSocket {
 public:
  UDSocket();
  UDSocket(void *context, int socket_fd);
  explicit UDSocket(int socket_fd);
  ~UDSocket();

  bool operator==(const UDSocket &other) const {
    return GetFd() == other.GetFd();
  }

  template <typename Q = void *>
  Q GetVaddr() const {
    return reinterpret_cast<Q>(reinterpret_cast<uintptr_t>(this));
  }

  int GetFd() const;
  bool IsListening() const;
  bool IsConnected() const;
  bool IsAlive() const;

  void SetNonBlocking() const;
  bool Listen(const std::string &path);
  bool Connect(const std::string &path);

  bool SendMsg(const char *msg, size_t len);
  bool SendMsgWithFd(const char *msg, size_t len, int fd);
  int RecvMsgWithFd(char *msg, size_t len, int *fd);

  void *GetContext() const { return context_; }
  bool AllocateUserData(size_t size);
  template <typename Q = void *>
  Q GetUserData() const {
    return reinterpret_cast<Q>(user_data_);
  }

  void Shutdown();

 private:
  void *context_{nullptr};
  int socket_fd_;
  bool is_listening_;
  bool is_connected_;
  void *user_data_{nullptr};
};

}  // namespace net
}  // namespace juggler

namespace std {
template <>
struct hash<juggler::net::UDSocket> {
  size_t operator()(const juggler::net::UDSocket &socket) const {
    // File descriptor is unique for each socket.
    return socket.GetFd();
  }
};
}  // namespace std

namespace juggler {
namespace net {

class UDServer {
 public:
  using on_connect_cb_t = std::function<bool(UDSocket *)>;
  using on_close_cb_t = std::function<void(UDSocket *)>;
  using on_message_cb_t =
      std::function<void(UDSocket *, const char *, size_t, int)>;
  using on_timeout_cb_t = std::function<void(UDSocket *)>;
  UDServer(const std::string &path, on_connect_cb_t on_connect,
           on_close_cb_t on_close, on_message_cb_t on_message,
           on_timeout_cb_t on_timeout);
  ~UDServer();

  void Run();
  void Stop();

 private:
  const on_connect_cb_t on_connect_;
  const on_close_cb_t on_close_;
  const on_message_cb_t on_message_;
  const on_timeout_cb_t on_timeout_;
  std::atomic<bool> keep_running_;
  UDSocket listen_socket_;
  std::unordered_map<int, std::unique_ptr<UDSocket>> connected_clients_;
};

}  // namespace net
}  // namespace juggler

#endif  // SRC_INCLUDE_UD_SOCKET_H_
