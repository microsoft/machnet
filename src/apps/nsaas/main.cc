/**
 * @file main.cc
 * @brief NSaaS stack main entry point.
 */
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <nsaas_controller.h>

DEFINE_string(config_json, "../../../config.json",
              "JSON file with NSaaS-related parameters.");

int main(int argc, char *argv[]) {
  ::google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  gflags::SetUsageMessage("Main NSaaS daemon.");

  juggler::NSaaSController *controller =
      CHECK_NOTNULL(juggler::NSaaSController::Create(FLAGS_config_json));
  controller->Run();

  juggler::NSaaSController::ReleaseInstance();
  return (0);
}
