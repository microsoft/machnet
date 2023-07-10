/**
 * @file main.cc
 * Simple hello world application using only NSaaS public APIs
 * Usage:
 *  - First start the server: ./hello_world --local=<local IP>
 *  - Client:
 *    ./hello_world --local=<local IP> --remote=<server IP> --is_client=1
 */

#include <gflags/gflags.h>
#include <nsaas.h>

#include <array>

DEFINE_string(local, "", "Local IP address");
DEFINE_string(remote, "", "Remote IP address");

static constexpr uint16_t kPort = 31580;

// assert with message
void assert_with_msg(bool cond, const char *msg) {
  if (!cond) {
    printf("%s\n", msg);
    exit(-1);
  }
}

int main(int argc, char *argv[]) {
  google::ParseCommandLineFlags(&argc, &argv, true);

  int ret = nsaas_init();
  assert_with_msg(ret == 0, "nsaas_init() failed");

  void *channel = nsaas_attach();
  assert_with_msg(channel != nullptr, "nsaas_attach() failed");

  ret = nsaas_listen(channel, FLAGS_local.c_str(), kPort);
  assert_with_msg(ret == 0, "nsaas_listen() failed");

  printf("Listening on %s:%d\n", FLAGS_local.c_str(), kPort);

  if (FLAGS_remote != "") {
    printf("Sending message to %s:%d\n", FLAGS_remote.c_str(), kPort);
    NSaaSNetFlow flow;
    std::string msg = "Hello World!";
    ret = nsaas_connect(channel, FLAGS_local.c_str(), FLAGS_remote.c_str(),
                        kPort, &flow);
    assert_with_msg(ret == 0, "nsaas_connect() failed");

    const int ret = nsaas_send(channel, flow, msg.data(), msg.size());
    if (ret == -1) printf("nsaas_send() failed\n");
  } else {
    printf("Waiting for message from client\n");
    size_t count = 0;

    while (true) {
      std::array<char, 1024> buf;
      NSaaSNetFlow flow;
      const ssize_t ret = nsaas_recv(channel, buf.data(), buf.size(), &flow);
      assert_with_msg(ret >= 0, "nsaas_recvmsg() failed");
      if (ret == 0) {
        usleep(10);
        continue;
      }

      std::string msg(buf.data(), ret);
      printf("Received message: %s, count = %zu\n", msg.c_str(), count++);
    }
  }

  return 0;
}
