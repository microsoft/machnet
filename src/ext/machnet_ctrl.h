#ifndef SRC_EXT_MACHNET_CTRL_H_
#define SRC_EXT_MACHNET_CTRL_H_

#include "machnet_common.h"
#ifdef __cplusplus
extern "C" {
#endif

#include <uuid/uuid.h>

#define MACHNET_CONTROLLER_DEFAULT_PATH "/var/run/machnet/machnet_ctrl.sock"

/**
 * @struct machnet_app_info
 * @brief  Information about the application
 * @var    machnet_app_info::name Name of the application.
 */
struct machnet_app_info {
  char name[128];
} __attribute__((packed));
typedef struct machnet_app_info machnet_app_info_t;

/**
 * @struct machnet_channel_info
 * @brief This struct is used to request a new channel from the controller.
 * An application that needs to use Machnet should send a request to create a
 * new channel to the controller.
 *
 * @var machnet_channel_info::channel_uuid     The UUID of the application that
 * is requesting a new channel.
 * @var machnet_channel_info::desc_ring_size   The depth of the descriptor rings
 * (Machnet, App).
 * @var machnet_channel_info::buffer_count     The size of the buffer pool.
 */
struct machnet_channel_info {
  uuid_t channel_uuid;
#define MACHNET_CHANNEL_INFO_DESC_RING_SIZE_DEFAULT 1024
  uint32_t desc_ring_size;
#define MACHNET_CHANNEL_INFO_BUFFER_COUNT_DEFAULT 4096
  uint32_t buffer_count;
} __attribute__((packed));
typedef struct machnet_channel_info machnet_channel_info_t;

/**
 * @struct machnet_ctrl_resp
 */
struct machnet_ctrl_status {
#define MACHNET_CTRL_STATUS_FAILURE -1
#define MACHNET_CTRL_STATUS_SUCCESS 0
  int status;
};
typedef struct machnet_ctrl_status machnet_ctrl_status_t;

/**
 * @struct machnet_ctrl_msg
 */
struct machnet_ctrl_msg {
#define MACHNET_CTRL_MSG_TYPE_INVALID 0x00
#define MACHNET_CTRL_MSG_TYPE_REQ_REGISTER 0x01
#define MACHNET_CTRL_MSG_TYPE_REQ_CHANNEL 0x02
#define MACHNET_CTRL_MSG_TYPE_REQ_FLOW 0x03
#define MACHNET_CTRL_MSG_TYPE_REQ_LISTEN 0x04
#define MACHNET_CTRL_MSG_TYPE_RESPONSE 0x10
  uint16_t type;
  uint32_t msg_id;
  uuid_t app_uuid;
  int status;
  union {
    machnet_app_info_t app_info;
    machnet_channel_info_t channel_info;
  };
} __attribute__((packed));
typedef struct machnet_ctrl_msg machnet_ctrl_msg_t;

extern uuid_t g_app_uuid;

#ifdef __cplusplus
}
#endif

#endif  // SRC_EXT_MACHNET_CTRL_H_
