#include <config.h>
#include <dpdk.h>
#include <glog/logging.h>
#include <machnet_controller.h>
#include <machnet_ctrl.h>
#include <utils.h>
#include <worker.h>

#include <future>
#include <memory>
#include <thread>

namespace juggler {

struct MachnetClientContext {
  bool registered;
  uuid_t uuid;
};

MachnetController::MachnetController(const std::string &conf_file)
    : config_processor_{conf_file}, channel_manager_{} {}

void MachnetController::Run() {
  if (IsRunning()) {
    LOG(ERROR) << "Controller is already running.";
    return;
  }

  if (config_processor_.interfaces_config().empty()) {
    LOG(ERROR) << "No interfaces configured. Exiting.";
    return;
  }

  signal(SIGINT, MachnetController::sig_handler);

  // Initialize DPDK.
  dpdk_.InitDpdk(config_processor_.GetEalOpts());
  if (dpdk_.GetNumPmdPortsAvailable() == 0) {
    LOG(ERROR) << "Error: No DPDK-capable ports found. This can be due to "
                  "several reasons.";
    LOG(ERROR) << "1. On Azure, the accelerated NIC must first be unbound from "
                  "the kernel driver with driverctl";
    LOG(ERROR) << "2. The user libraries for the NIC are not installed, e.g., "
                  "libmlx5 for Mellanox NICs";
    LOG(ERROR) << "3. The NIC is not supported by DPDK";
    return;
  }

  std::vector<cpu_set_t> cpu_masks;
  // Find the PMD port id for each interface.
  for (const auto &interface : config_processor_.interfaces_config()) {
    auto pmd_port_id = dpdk_.GetPmdPortIdByMac(interface.l2_addr());
    if (!pmd_port_id) {
      LOG(ERROR) << "Cannot find PMD port id for interface with L2 address: "
                 << interface.l2_addr().ToString();
      return;
    }
    const_cast<NetworkInterfaceConfig &>(interface).set_dpdk_port_id(
        pmd_port_id.value());

    // Initialize the PMD port.
    const uint16_t rx_rings_nr = interface.engine_threads(),
                   tx_rings_nr = interface.engine_threads();
    pmd_ports_.emplace_back(std::make_shared<juggler::dpdk::PmdPort>(
        interface.dpdk_port_id().value(), rx_rings_nr, tx_rings_nr,
        dpdk::PmdRing::kDefaultRingDescNr, dpdk::PmdRing::kDefaultRingDescNr));
    pmd_ports_.back()->InitDriver();

    // Create the MachnetEngineShared State.
    auto shared_state = std::make_shared<MachnetEngineSharedState>(
        pmd_ports_.back()->GetRSSKey(), pmd_ports_.back()->GetL2Addr(),
        std::vector<net::Ipv4::Address>(1, interface.ip_addr()));
    // Create the Machnet engines.
    for (size_t i = 0; i < interface.engine_threads(); ++i) {
      engines_.emplace_back(std::make_shared<juggler::MachnetEngine>(
          pmd_ports_.back(), i, i, shared_state));
      // Create the CPU mask for the engine threads.
      cpu_masks.emplace_back(interface.cpu_mask());
    }
  }

  WorkerPool<MachnetEngine> engine_thread_pool{engines_, cpu_masks};
  engine_thread_pool.Init();
  engine_thread_pool.Launch();

  // Start the controller server, wait and handle connections.
  RunController();

  // The previous call will block until the server is stopped (e.g. by SIGINT).
  engine_thread_pool.Pause();
  engine_thread_pool.Terminate();

  for (const auto &pmd_port : pmd_ports_) {
    pmd_port->DumpStats();
  }
}

bool MachnetController::HandleNewConnection(UDSocket *s) {
  // Client context.
  if (!s->AllocateUserData(sizeof(MachnetClientContext))) return false;
  auto *client_context =
      reinterpret_cast<MachnetClientContext *>(s->GetUserData());
  CHECK_NOTNULL(client_context);
  client_context->registered = false;
  return true;
}

void MachnetController::HandleNewMessage(UDSocket *s, const char *data,
                                         size_t length) {
  CHECK_NOTNULL(s);
  if (length != sizeof(machnet_ctrl_msg_t)) {
    LOG(ERROR) << "Invalid message length";
    return;
  }

  auto *req = reinterpret_cast<const machnet_ctrl_msg_t *>(data);
  switch (req->type) {
    case MACHNET_CTRL_MSG_TYPE_REQ_REGISTER: {
      machnet_ctrl_msg_t resp;
      resp.type = MACHNET_CTRL_MSG_TYPE_RESPONSE;
      resp.msg_id = req->msg_id;

      auto ret = RegisterApplication(req->app_uuid, &req->app_info);
      resp.status =
          ret ? MACHNET_CTRL_STATUS_SUCCESS : MACHNET_CTRL_STATUS_FAILURE;
      CHECK(s->SendMsg(reinterpret_cast<char *>(&resp), sizeof(resp)));

      // Get client context.
      auto *client_context =
          reinterpret_cast<MachnetClientContext *>(s->GetUserData());
      client_context->registered = true;
      juggler::utils::Copy(client_context->uuid, req->app_uuid, sizeof(uuid_t));
    } break;
    case MACHNET_CTRL_MSG_TYPE_REQ_CHANNEL: {
      LOG(INFO) << "Request to create new channel: "
                << juggler::utils::UUIDToString(req->channel_info.channel_uuid);
      int channel_fd;
      auto ret = CreateChannel(req->app_uuid, &req->channel_info, &channel_fd);

      machnet_ctrl_msg_t resp;
      resp.type = MACHNET_CTRL_MSG_TYPE_RESPONSE;
      resp.msg_id = req->msg_id;

      if (ret && channel_fd >= 0) {
        resp.status = MACHNET_CTRL_STATUS_SUCCESS;
        LOG(INFO) << "Sending channel fd: " << channel_fd << " to client.";
        CHECK(s->SendMsgWithFd(reinterpret_cast<char *>(&resp), sizeof(resp),
                               channel_fd));
      } else {
        resp.status = MACHNET_CTRL_STATUS_FAILURE;
        CHECK(s->SendMsg(reinterpret_cast<char *>(&resp), sizeof(resp)));
      }
    } break;
    default:
      LOG(ERROR) << "Invalid message type.";
      break;
  }
}

void MachnetController::HandlePassiveClose(UDSocket *s) {
  // Get client context.
  auto *client_context =
      reinterpret_cast<MachnetClientContext *>(s->GetUserData());
  if (client_context->registered) {
    LOG(INFO) << "Client " << juggler::utils::UUIDToString(client_context->uuid)
              << " disconnected.";

    UnregisterApplication(client_context->uuid);
    client_context->registered = false;
  }
}

void MachnetController::HandleTimeout(UDSocket *s) {
  // TODO(ilias): Handle timeout.
  LOG(WARNING) << "Not implemented.";
}

bool MachnetController::RegisterApplication(
    const uuid_t app_uuid, const machnet_app_info_t *app_info) {
  const std::string app_uuid_str = juggler::utils::UUIDToString(app_uuid);

  // Check if the application is already registered.
  if (applications_registered_.find(app_uuid_str) !=
      applications_registered_.end()) {
    LOG(ERROR) << "Application is already registered.";
    return false;
  }

  // Register the application.
  applications_registered_.insert({app_uuid_str, {}});
  LOG(INFO) << "Application registered: " << app_uuid_str;

  return true;
}

void MachnetController::UnregisterApplication(const uuid_t app_uuid) {
  const std::string app_uuid_str = juggler::utils::UUIDToString(app_uuid);

  // Check if the application is registered.
  if (applications_registered_.find(app_uuid_str) ==
      applications_registered_.end()) {
    LOG(ERROR) << "Application is not registered.";
    return;
  }

  // TODO(ilias): Destroy all channels and flows.
  const auto &app_channels = applications_registered_[app_uuid_str];

  for (const auto &channel_name : app_channels) {
    LOG(INFO) << "Destroying channel: " << channel_name;
    auto channel = channel_manager_.GetChannel(channel_name.c_str());
    static size_t engine_index =
        utils::hash<size_t>(channel->GetName().c_str(),
                            channel->GetName().size()) %
        engines_.size();
    engines_[engine_index]->RemoveChannel(channel);
    channel_manager_.DestroyChannel(channel_name.c_str());
  }

  // Unregister the application.
  applications_registered_.erase(app_uuid_str);
  LOG(INFO) << "Application unregistered: " << app_uuid_str;
}

bool MachnetController::CreateChannel(
    const uuid_t app_uuid, const machnet_channel_info_t *channel_info,
    int *fd) {
  const std::string app_uuid_str = juggler::utils::UUIDToString(app_uuid);

  // Check that this is a registered application.
  if (applications_registered_.find(app_uuid_str) ==
      applications_registered_.end()) {
    LOG(ERROR) << "Application not registered: " << app_uuid_str;
    return false;
  }

  const std::string channel_uuid_str =
      juggler::utils::UUIDToString(channel_info->channel_uuid);
  auto &app_channels = applications_registered_[app_uuid_str];
  if (app_channels.find(channel_uuid_str) != app_channels.end()) {
    LOG(ERROR) << "Channel already registered: " << channel_uuid_str;
    return false;
  }

  // TODO(ilias): Figure out a way to dynamically infer the buffer size to be
  // used.
  const auto channel_buffer_size =
      juggler::dpdk::PmdRing::kDefaultFrameSize - sizeof(juggler::net::Ipv4) -
      sizeof(juggler::net::Udp) - sizeof(juggler::net::MachnetPktHdr);
  if (!channel_manager_.AddChannel(
          channel_uuid_str.c_str(), ChannelManager::kDefaultRingSize,
          ChannelManager::kDefaultRingSize, ChannelManager::kDefaultBufferCount,
          channel_buffer_size) != 0) {
    return false;
  }

  // Add the channel to the list of channels for this application.
  app_channels.insert(channel_uuid_str);

  // Pass a promise to the Machnet engine and wait for the channel to be
  // activated.
  std::promise<bool> p;
  auto fstatus = p.get_future();
  static size_t engine_index =
      utils::hash<size_t>(channel_uuid_str.c_str(), channel_uuid_str.size()) %
      engines_.size();
  const auto &engine = engines_[engine_index];
  engine->AddChannel(
      CHECK_NOTNULL(channel_manager_.GetChannel(channel_uuid_str.c_str())),
      std::move(p));

  // TODO(ilias): Add a timeout here.
  auto status = fstatus.get();

  if (status != true) {
    LOG(ERROR) << "Failed to create channel.";
    *fd = -1;
    return false;
  }

  if (kShmZeroCopyEnabled) {
    LOG(INFO) << "Registering channel buffer memory with NIC DPDK driver.";
    // Register channel buffer memory with NIC DPDK driver.
    auto device = engine->GetPmdPort()->GetDevice();
    CHECK(channel_manager_.GetChannel(channel_uuid_str.c_str())
              ->RegisterMemForDMA(device));
  } else {
    LOG(INFO) << "Not registering channel buffer memory with NIC DPDK driver.";
  }

  *fd = channel_manager_.GetChannel(channel_uuid_str.c_str())->GetFd();
  return status;
}

void MachnetController::RunController() {
  const std::string socket_path = MACHNET_CONTROLLER_DEFAULT_PATH;

  const UDServer::on_connect_cb_t on_connect_cb = std::bind(
      &MachnetController::HandleNewConnection, this, std::placeholders::_1);

  const UDServer::on_close_cb_t on_close_cb = [=, this](UDSocket *socket) {
    this->HandlePassiveClose(socket);
  };

  // UDServer::on_close_cb_t on_close_cb = std::bind(
  //     &MachnetController::HandlePassiveClose, this, std::placeholders::_1);
  const UDServer::on_message_cb_t on_message_cb =
      [=, this](UDSocket *socket, const char *data, size_t length, int fd) {
        this->HandleNewMessage(socket, data, length);
      };

  const UDServer::on_timeout_cb_t on_timeout_cb = [=, this](UDSocket *socket) {
    this->HandleTimeout(socket);
  };

  server_ = std::make_unique<UDServer>(socket_path, on_connect_cb, on_close_cb,
                                       on_message_cb, on_timeout_cb);

  server_->Run();
}

void MachnetController::Stop() {
  CHECK_NOTNULL(server_);
  server_->Stop();
}

}  // namespace juggler
