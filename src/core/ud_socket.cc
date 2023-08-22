#include <fcntl.h>
#include <glog/logging.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <ud_socket.h>
#include <unistd.h>
#include <utils.h>

#include <filesystem>
#include <thread>

namespace juggler {
namespace net {

UDSocket::UDSocket()
    : socket_fd_(-1), is_listening_(false), is_connected_(false) {
  socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd_ == -1) {
    LOG(FATAL) << "Failed to create socket";
  }
}

UDSocket::UDSocket(int socket_fd) : UDSocket(nullptr, socket_fd) {}

UDSocket::UDSocket(void *context, int socket_fd)
    : context_(context),
      socket_fd_(-1),
      is_listening_(false),
      is_connected_(false) {
  if (socket_fd < 0) {
    LOG(FATAL) << "Invalid socket fd";
  } else {
    socket_fd_ = socket_fd;
  }
}

UDSocket::~UDSocket() {
  if (user_data_ != nullptr) {
    free(user_data_);
    user_data_ = nullptr;
  }
  Shutdown();
}

int UDSocket::GetFd() const { return socket_fd_; }

bool UDSocket::IsListening() const { return is_listening_; }

bool UDSocket::IsConnected() const { return is_connected_; }

bool UDSocket::IsAlive() const {
  int error = 0;
  socklen_t len = sizeof(error);
  auto retval = getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &error, &len);
  if (retval != 0) {
    LOG(ERROR) << "Failed to get socket error code.";
    return false;
  }

  if (error != 0) {
    LOG(ERROR) << "Socket error code: " << error;
    return false;
  }

  return true;
}

void UDSocket::SetNonBlocking() const {
  int flags = fcntl(socket_fd_, F_GETFL, 0);
  if (flags == -1) {
    LOG(FATAL) << "Failed to get socket flags";
  }
  flags |= O_NONBLOCK;
  if (fcntl(socket_fd_, F_SETFL, flags) == -1) {
    LOG(FATAL) << "Failed to set socket flags";
  }
}

bool UDSocket::Listen(const std::string &path) {
  if (IsListening()) {
    LOG(ERROR) << "Socket is already listening";
    return false;
  }

  // Remove the socket file if it already exists.
  if (access(path.c_str(), F_OK) != -1) {
    if (unlink(path.c_str()) == -1) {
      LOG(ERROR) << "Failed to remove socket file " << path
                 << ", error code: " << strerror(errno);
      return false;
    }
  }

  // Get the leaf directory of the socket file.
  const auto directory = std::filesystem::path{path}.parent_path().string();
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    LOG(ERROR) << "Failed to create directory " << directory
               << ", error code: " << ec.message();
    return false;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (bind(socket_fd_, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    LOG(ERROR) << "Failed to bind Unix domain socket to path: " << path
               << ", error code: " << strerror(errno);
    return false;
  }

  // Change the permission of the socket file.
  chmod(path.c_str(), 0777);

  const int backlog = 5;
  if (listen(socket_fd_, backlog) == -1) {
    LOG(FATAL) << "Failed to listen on socket";
    return false;
  }
  is_listening_ = true;
  return is_listening_;
}

bool UDSocket::Connect(const std::string &path) {
  if (IsConnected()) {
    LOG(ERROR) << "Socket is already connected";
    return false;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (connect(socket_fd_, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    LOG(ERROR) << "Failed to connect to socket";
    return false;
  }
  is_connected_ = true;
  return is_connected_;
}

bool UDSocket::SendMsg(const char *msg, size_t len) {
  msghdr msg_hdr;
  memset(&msg_hdr, 0, sizeof(msg_hdr));
  iovec iov;
  iov.iov_base = const_cast<char *>(msg);
  iov.iov_len = len;
  msg_hdr.msg_iov = &iov;
  msg_hdr.msg_iovlen = 1;
  if (sendmsg(socket_fd_, &msg_hdr, 0) == -1) {
    LOG(ERROR) << "Failed to send message";
    return false;
  }
  return true;
}

bool UDSocket::SendMsgWithFd(const char *msg, size_t len, int fd) {
  msghdr msg_hdr;
  memset(&msg_hdr, 0, sizeof(msg_hdr));
  iovec iov;
  iov.iov_base = const_cast<char *>(msg);
  iov.iov_len = len;
  msg_hdr.msg_iov = &iov;
  msg_hdr.msg_iovlen = 1;
  char buf[CMSG_SPACE(sizeof(int))];
  memset(buf, 0, sizeof(buf));
  msg_hdr.msg_control = buf;
  msg_hdr.msg_controllen = sizeof(buf);
  cmsghdr *cmsg = CMSG_FIRSTHDR(&msg_hdr);
  cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  *(reinterpret_cast<int *>(CMSG_DATA(cmsg))) = fd;
  if (sendmsg(socket_fd_, &msg_hdr, 0) == -1) {
    LOG(ERROR) << "Failed to send message";
    return false;
  }
  return true;
}

int UDSocket::RecvMsgWithFd(char *msg, size_t len, int *fd) {
  *fd = -1;  // Set to invalid fd.

  msghdr msg_hdr;
  memset(&msg_hdr, 0, sizeof(msg_hdr));
  iovec iov;
  iov.iov_base = msg;
  iov.iov_len = len;
  msg_hdr.msg_iov = &iov;
  msg_hdr.msg_iovlen = 1;
  char buf[CMSG_SPACE(sizeof(int))];
  memset(buf, 0, sizeof(buf));
  msg_hdr.msg_control = buf;
  msg_hdr.msg_controllen = sizeof(buf);

  int nbytes = recvmsg(socket_fd_, &msg_hdr, 0);
  if (nbytes == -1) {
    LOG(ERROR) << "Failed to receive message";
    return nbytes;
  }

  cmsghdr *cmsg = CMSG_FIRSTHDR(&msg_hdr);
  if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
    if (cmsg->cmsg_level != SOL_SOCKET) {
      LOG(ERROR) << "Invalid cmsg_level " << cmsg->cmsg_level;
      return nbytes;
    }
    if (cmsg->cmsg_type != SCM_RIGHTS) {
      LOG(ERROR) << "Invalid cmsg_type " << cmsg->cmsg_type;
      return nbytes;
    }
    *fd = *(reinterpret_cast<int *>(CMSG_DATA(cmsg)));
  }

  return nbytes;
}

bool UDSocket::AllocateUserData(size_t size) {
  if (user_data_ != nullptr) {
    LOG(ERROR) << "User data already allocated";
    return false;
  }
  user_data_ = malloc(size);
  if (!user_data_) {
    LOG(ERROR) << "Failed to allocate user data";
    return false;
  }
  return true;
}

void UDSocket::Shutdown() {
  if (socket_fd_ != -1) {
    close(socket_fd_);
    socket_fd_ = -1;
  }
  is_listening_ = false;
  is_connected_ = false;
}

UDServer::UDServer(const std::string &path, on_connect_cb_t on_connect,
                   on_close_cb_t on_close, on_message_cb_t on_message,
                   on_timeout_cb_t on_timeout)
    : on_connect_(CHECK_NOTNULL(on_connect)),
      on_close_(CHECK_NOTNULL(on_close)),
      on_message_(CHECK_NOTNULL(on_message)),
      on_timeout_(CHECK_NOTNULL(on_timeout)),
      keep_running_(false),
      listen_socket_(),
      connected_clients_() {
  if (!listen_socket_.Listen(path)) {
    LOG(FATAL) << "Failed to listen on socket";
  }
}

UDServer::~UDServer() {
  // No need to close the listen or connected sockets; they will be closed when
  // the object is destroyed.
}

void UDServer::Run() {
  // Poll the listening socket for new connections.
  int epoll_fd = epoll_create1(0);
  if (epoll_fd == -1) {
    LOG(FATAL) << utils::Format("Failed to create epoll fd (%s)",
                                strerror(errno));
    // Program will exit here.
  }

  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = listen_socket_.GetFd();
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_socket_.GetFd(), &ev) == -1) {
    LOG(FATAL) << utils::Format("Failed to add listen socket to epoll (%s)",
                                strerror(errno));
    // Program will exit here.
  }
  const size_t kMaxEvents = 16;
  std::vector<struct epoll_event> event_list(kMaxEvents);
  const size_t kMaxBufferSize = 4096;
  std::vector<uint8_t> buffer(kMaxBufferSize);

  keep_running_.store(true);
  const int kEpollTimeoutMs = 10;
  while (keep_running_.load()) {
    auto nfds = epoll_wait(epoll_fd, event_list.data(), event_list.size(),
                           kEpollTimeoutMs);
    if (nfds == -1 && errno != EINTR) {
      LOG(FATAL) << utils::Format("Failed to wait on epoll (%s)",
                                  strerror(errno));
      // Program will exit here.
    }

    for (int i = 0; i < nfds; ++i) {
      if (event_list[i].data.fd == listen_socket_.GetFd()) {
        // New connection.
        int new_fd = accept(listen_socket_.GetFd(), nullptr, nullptr);
        if (new_fd == -1) {
          LOG(ERROR) << utils::Format("Failed to accept new connection (%s)",
                                      strerror(errno));
          continue;
        }

        auto client = std::make_unique<UDSocket>(new_fd);

        // Call the on_connect callback.
        LOG(INFO) << "New connection";
        if (!on_connect_(client.get())) {
          close(new_fd);
          continue;
        }

        // Add the new connection to the list of connected clients, and register
        // it with epoll.
        ev.events = EPOLLIN;
        ev.data.fd = new_fd;
        connected_clients_.insert({new_fd, std::move(client)});
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_fd, &ev) == -1) {
          LOG(ERROR) << utils::Format(
              "Failed to add new connection to epoll (%s)", strerror(errno));
          close(new_fd);
          connected_clients_.erase(new_fd);
          continue;
        }
      } else if (event_list[i].events & EPOLLIN) {
        // Existing connection.
        UDSocket *socket = connected_clients_[event_list[i].data.fd].get();
        int received_fd;

        // Read the message.
        auto nbytes =
            socket->RecvMsgWithFd(reinterpret_cast<char *>(buffer.data()),
                                  buffer.size(), &received_fd);
        if (nbytes == -1) {
          LOG(ERROR) << "Failed to receive message";
          // Close the socket.
          on_close_(socket);
          connected_clients_.erase(socket->GetFd());
        } else if (nbytes == 0) {
          // Client disconnected.
          on_close_(socket);
          connected_clients_.erase(socket->GetFd());
        } else {
          // New message from client; call the relevant callback.
          on_message_(socket, reinterpret_cast<char *>(buffer.data()), nbytes,
                      received_fd);
        }
      }
    }
  }

  // Allow the server to be shut down gracefully.
  for (auto &client : connected_clients_) {
    const auto &socket = client.second;
    // Call the on_close callback.
    on_close_(socket.get());
  }

  close(epoll_fd);
}

void UDServer::Stop() {
  keep_running_ = false;
  keep_running_.store(false);
  // Sleep for a short time to allow the server to shut down gracefully.
}

}  // namespace net
}  // namespace juggler
