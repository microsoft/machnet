# Declare the library target.
set(MACHNET_SHIM_LIB_NAME machnet_shim)

add_library(${MACHNET_SHIM_LIB_NAME} SHARED machnet.c)
target_link_libraries (${MACHNET_SHIM_LIB_NAME} uuid)
target_link_libraries (${MACHNET_SHIM_LIB_NAME} uuid)

# Configure the directories to search for header files.
target_include_directories(${MACHNET_SHIM_LIB_NAME} PRIVATE .)
target_include_directories(${MACHNET_SHIM_LIB_NAME} PRIVATE ../include)

# Set the version property.
set_target_properties(${MACHNET_SHIM_LIB_NAME} PROPERTIES VERSION ${PROJECT_VERSION})

# Set the shared object version property to the project's major version.
set_target_properties(${MACHNET_SHIM_LIB_NAME} PROPERTIES SOVERSION ${PROJECT_VERSION_MAJOR})

# Set the public header property to the one with the actual API.
set_target_properties(${MACHNET_SHIM_LIB_NAME} PROPERTIES PUBLIC_HEADER machnet.h)
