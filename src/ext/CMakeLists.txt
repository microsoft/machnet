include_directories(${USOCKETS_SDK}/src)
include_directories(${UV_SDK}/include)

# Declare the library target.
set(NSAAS_SHIM_LIB_NAME nsaas_shim)

add_library(${NSAAS_SHIM_LIB_NAME} SHARED nsaas.c)
target_link_libraries (${NSAAS_SHIM_LIB_NAME} uuid)

# Configure the directories to search for header files.
target_include_directories(${NSAAS_SHIM_LIB_NAME} PRIVATE .)

# Set the version property.
set_target_properties(${NSAAS_SHIM_LIB_NAME} PROPERTIES VERSION ${PROJECT_VERSION})

# Set the shared object version property to the project's major version.
set_target_properties(${NSAAS_SHIM_LIB_NAME} PROPERTIES SOVERSION ${PROJECT_VERSION_MAJOR})

# Set the public header property to the one with the actual API.
set_target_properties(${NSAAS_SHIM_LIB_NAME} PROPERTIES PUBLIC_HEADER nsaas.h)
