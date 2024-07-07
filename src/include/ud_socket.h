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
#include <machnet_ctrl.h>

#define ASIO_STANDLONE
#include <asio.hpp>
#if defined(ASIO_HAS_LOCAL_SOCKETS)
using asio::local::stream_protocol;

namespace juggler {
namespace net {

class UDServer;

class UDSocket : public std::enable_shared_from_this<UDSocket> {
public:
    using on_connect_cb_t = std::function<bool(UDSocket *)>;
    using on_message_cb_t = std::function<void(UDSocket *, const char *, size_t, int)>;
    using on_close_cb_t = std::function<void(UDSocket *)>;
    
    UDSocket(stream_protocol::socket socket, on_connect_cb_t on_connect, 
        on_message_cb_t on_message, on_close_cb_t on_close);

    ~UDSocket();

    void start();
    void do_read();
    bool SendMsg(const char *msg, size_t len);
    bool SendMsgWithFd(const char *msg, size_t len, int fd);
    bool AllocateUserData(size_t size);
    template <typename Q = void *>
    Q GetUserData() const {
        printf("inside ud_socket GetUserData\n");
        if(user_data_ == NULL) printf("user_data_ is NULL\n");
        else printf("user_data_ is not NULL\n");
        return reinterpret_cast<Q>(user_data_);
    }
    
private:
    stream_protocol::socket socket_;
    machnet_ctrl_msg_t req, resp;
    const on_connect_cb_t on_connect_;
    const on_close_cb_t on_close_;
    const on_message_cb_t on_message_;
    void *user_data_{nullptr};
};

class UDServer {
public:
    using on_connect_cb_t = std::function<bool(UDSocket *)>;
    using on_close_cb_t = std::function<void(UDSocket *)>;
    using on_message_cb_t = std::function<void(UDSocket *, const char *, size_t, int)>;
    using on_timeout_cb_t = std::function<void(UDSocket *)>;
    
    UDServer(asio::io_context &io_context, const std::string &path, 
        on_connect_cb_t on_connect, on_close_cb_t on_close, 
        on_message_cb_t on_message, on_timeout_cb_t on_timeout);

    void do_accept();

private:
    stream_protocol::acceptor acceptor_;
    const on_connect_cb_t on_connect_;
    const on_close_cb_t on_close_;
    const on_message_cb_t on_message_;
    const on_timeout_cb_t on_timeout_;
};

}  // namespace net
}  // namespace juggler

#else // defined(ASIO_HAS_LOCAL_SOCKETS)
# error Local sockets not available on this platform.
#endif // defined(ASIO_HAS_LOCAL_SOCKETS)

#endif  // SRC_INCLUDE_UD_SOCKET_H_
