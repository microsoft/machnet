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

// adding iostream for debugging
#include <iostream>

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
    on_message_(on_message) {
        std::cout << "inside UDSocket constructor" << std::endl;
    }

UDSocket::~UDSocket() {
    on_close_(this);
}

void UDSocket::start() {
    std::cout << "inside UDSocket::start() and firing on_connect_ cb" << std::endl;
    on_connect_(this);
    do_read();
}

bool UDSocket::AllocateUserData(size_t size) {
    std::cout << "Inside AllocateUserData" << std::endl;
    if (user_data_ != nullptr) {
        std::cout << "User data already allocated" << std::endl;
        LOG(ERROR) << "User data already allocated";
        return false;
    }
    user_data_ = malloc(size);
    if (!user_data_) {
        std::cout << "Failed to allocate user data of size: " << size << std::endl;
        LOG(ERROR) << "Failed to allocate user data";
        return false;
    }
    return true;
}

void UDSocket::do_read() {
    std::cout << "inside UDSocket::do_read() " << std::endl;
    auto self(shared_from_this());
    asio::async_read(socket_, 
        asio::buffer(reinterpret_cast<char *>(&req), sizeof(req)),
        [this, self](std::error_code ec, std::size_t bytes_transferred) {
            if (!ec) {
                std::cout << "inside UDSocket::do_read() async_read lamda" << std::endl;
                std::cout << "before on_message_ cb, bytes_transferred: " << bytes_transferred << std::endl;
                on_message_(self.get(), reinterpret_cast<char *>(&req),
                    bytes_transferred, 0);
                std::cout << "after on_message_ cb in socket do_read async_read" << std::endl;
            }
            else {
                std::cout << "async_read error: " << ec.message() << std::endl;
                std::cout << "async_read error code: " << ec.value() << std::endl;
            }
        }
    );
}

bool UDSocket::SendMsg(const char *msg, size_t len) {
    std::cout << "inside SendMsg, len: " << len << std::endl;
    if(len != sizeof(machnet_ctrl_msg_t)) {
        LOG(ERROR) << "Invalid message length";
        return false;
    }
    std::cout << "After ctrl msg size check" << std::endl;

    auto self(shared_from_this());
    std::memcpy(&resp, msg, sizeof(resp));
    asio::async_write(socket_,
        asio::buffer(reinterpret_cast<char *>(&resp), sizeof(resp)),
        [this, self](std::error_code ec, std::size_t bytes_transferred) {
            if (!ec) {
                std::cout << "before calling do_read() from SendMsg async_write" << std::endl;
                std::cout << "bytes_transferred in async_write: " << bytes_transferred << std::endl;
                do_read();
                std::cout << "after do_read() in sendmsg async_write" << std::endl; 
            }
            else {
                std::cout << "SendMsg async_write error: " << ec.message() << std::endl;
                std::cout << "SendMsg async_write error code: " << ec.value() << std::endl;
            }
        }
    );

    return true;
}

bool UDSocket::SendMsgWithFd(const char *msg, size_t len, int fd) {
    std::cout << "inside SendMsgWithFd, calling UDSocket::SendMsg" << std::endl;
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

    std::cout << "inside UDServer constructor" << std::endl;
    LOG(INFO) << "UDServer constructor is called";
    do_accept();
}

void UDServer::do_accept() {
    std::cout << "inside UDServer do_accept" << std::endl;
    LOG(INFO) << "inside UDServer do_accept";
    acceptor_.async_accept(
        [this](std::error_code ec, stream_protocol::socket socket) {
            if (!ec) {
                std::cout << "inside server async_accept: calling socket start()" << std::endl;
                std::make_shared<UDSocket>(std::move(socket), on_connect_, on_message_, on_close_) -> start();
            }
            else {
                std::cout << "UDServer::do_accept() error: " << ec.message() << std::endl;
                std::cout << "UDServer::do_accept() error code: " << ec.value() << std::endl;
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
