set(target_name tcp_msg_gen)
add_executable(${target_name} main.cc)
target_compile_features(${target_name} PRIVATE cxx_std_17)
