#include <fcntl.h>
#include <glog/logging.h>

#ifdef __linux__
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#endif // __linux__

#include <ud_socket.h>
#include <unistd.h>
#include <utils.h>

#include <filesystem>
#include <thread>
#include <memory>
#include <functional>

#define ASIO_STANDLONE
#include <asio.hpp>
#if defined(ASIO_HAS_LOCAL_SOCKETS)
using asio::local::stream_protocol;

namespace juggler {
namespace net {

UDSocket::UDSocket(stream_protocol::socket socket, on_connect_cb_t on_connect, 
    on_message_cb_t on_message, on_close_cb_t on_close) 
    : socket_(std::move(socket)), 
    on_connect_(on_connect),
    on_close_(on_close),
    on_message_(on_message) {}

UDSocket::~UDSocket() {
    on_close_(this);
}

void UDSocket::start() {
    on_connect_(this);
    do_read();
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

void UDSocket::do_read() {
    auto self(shared_from_this());
    asio::async_read(socket_, 
        asio::buffer(reinterpret_cast<char *>(&req), sizeof(req)),
        [this, self](std::error_code ec, std::size_t bytes_transferred) {
            if (!ec) {
                on_message_(self.get(), reinterpret_cast<char *>(&req),
                    bytes_transferred, 0);
            }
        }
    );
}

bool UDSocket::SendMsg(const char *msg, size_t len) {
    if(len != sizeof(machnet_ctrl_msg_t)) {
        LOG(ERROR) << "Invalid message length";
        return false;
    }

    auto self(shared_from_this());
    std::memcpy(&resp, msg, sizeof(resp));
    asio::async_write(socket_,
        asio::buffer(reinterpret_cast<char *>(&resp), sizeof(resp)),
        [this, self](std::error_code ec, std::size_t bytes_transferred) {
            if (!ec) {
                do_read();
            }
        }
    );

    return true;
}

bool UDSocket::SendMsgWithFd(const char *msg, size_t len, int fd) {
    return SendMsg(msg, len);
}

UDServer::UDServer(asio::io_context &io_context, const std::string &path, 
    on_connect_cb_t on_connect, on_close_cb_t on_close, 
    on_message_cb_t on_message, on_timeout_cb_t on_timeout)
    : acceptor_(io_context, stream_protocol::endpoint(path), false /*reuse addr*/),
        on_connect_(CHECK_NOTNULL(on_connect)),
        on_close_(CHECK_NOTNULL(on_close)),
        on_message_(CHECK_NOTNULL(on_message)),
        on_timeout_(CHECK_NOTNULL(on_timeout)) {
    do_accept();
}

void UDServer::do_accept() {
    acceptor_.async_accept(
        [this](std::error_code ec, stream_protocol::socket socket) {
            if (!ec) {
                std::make_shared<UDSocket>(std::move(socket), on_connect_, on_message_, on_close_) -> start();
            }

            do_accept();
        }
    );
}

}  // namespace net
}  // namespace juggler

#else // defined(ASIO_HAS_LOCAL_SOCKETS)
# error Local sockets not available on this platform.
#endif // defined(ASIO_HAS_LOCAL_SOCKETS)
