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

  printf("inside app/machnet/main.cc\n");
  std::cout << FLAGS_config_json << std::endl;
  
  
  juggler::MachnetController *controller =
      CHECK_NOTNULL(juggler::MachnetController::Create(FLAGS_config_json));
  
  int flag = controller == NULL ? 0 : 1;
  std::cout << flag << " : _________from controller pointer" << std::endl;
  std::cout << "______________ calling controller -> run from machnet/main.cc" << std::endl;
  
  controller->Run();
  // system("pause");

  juggler::MachnetController::ReleaseInstance();
  return (0);
}
