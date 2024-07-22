/**
 * @file main.cc
 * @brief Machnet stack main entry point.
 */
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <machnet_controller.h>
#include <iostream>

DEFINE_string(config_json, "../src/apps/machnet/config.json",
              "JSON file with Machnet-related parameters.");

int main(int argc, char *argv[]) {
  printf("inside app/machnet/main.cc\n");
  ::google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  gflags::SetUsageMessage("Main Machnet daemon.");

  juggler::MachnetController *controller =
      CHECK_NOTNULL(juggler::MachnetController::Create(FLAGS_config_json));
  
  controller->Run();

  juggler::MachnetController::ReleaseInstance();
  return (0);
}
