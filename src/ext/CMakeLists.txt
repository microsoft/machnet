if (NOT DEFINED MACHNET_SHIM_EXPORT_STATIC)
	set(MACHNET_SHIM_EXPORT_STATIC FALSE)
endif()

# Declare the library target.
set(MACHNET_SHIM_LIB_NAME machnet_shim)

# List the sources
set(MACHNET_SHIM_SOURCES
    machnet.h
    machnet.cc
    machnet_export.h
)

if (MACHNET_SHIM_EXPORT_STATIC)
    # if static

else() 
    # dynamic
    add_library(${MACHNET_SHIM_LIB_NAME} SHARED ${MACHNET_SHIM_SOURCES})
endif()

# Treat the exported symbols as exported (and not imported) when building the library
target_compile_definitions(
    machnet_shim
	PRIVATE
    MACHNET_SHIM_EXPORT_BUILD
)

target_link_libraries (${MACHNET_SHIM_LIB_NAME} uuid)

# # Add ws2_32.lib for winsock APIs
if (WIN32)
    # find_library(ws2_32_LIBRARY_PATH ws2_32)
    target_link_libraries(${MACHNET_SHIM_LIB_NAME} msvcrt)
    target_link_libraries(${MACHNET_SHIM_LIB_NAME} ws2_32)
    # target_link_libraries(${MACHNET_SHIM_LIB_NAME} mswsock)
    target_link_libraries(${MACHNET_SHIM_LIB_NAME} rpcrt4)
# else ()
#     target_link_libraries (${MACHNET_SHIM_LIB_NAME} uuid)
endif (WIN32)

target_include_directories(${MACHNET_SHIM_LIB_NAME} PRIVATE .)
set_target_properties(${MACHNET_SHIM_LIB_NAME} PROPERTIES PUBLIC_HEADER machnet.h)
target_include_directories(machnet_shim
	INTERFACE
		${CMAKE_SOURCE_DIR}
)


# # _______________________________________________________________________________________________________________________
# # set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} MD")
# add_library(${MACHNET_SHIM_LIB_NAME} SHARED machnet.c)
# # set_property(TARGET ${MACHNET_SHIM_LIB_NAME} PROPERTY
# #       MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:DebugDLL")
# # set_target_properties(${MACHNET_SHIM_LIB_NAME} PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
# target_link_libraries (${MACHNET_SHIM_LIB_NAME} uuid)

# # # Add ws2_32.lib for winsock APIs
# if (WIN32)
#     # find_library(ws2_32_LIBRARY_PATH ws2_32)
#     target_link_libraries(${MACHNET_SHIM_LIB_NAME} msvcrt)
#     target_link_libraries(${MACHNET_SHIM_LIB_NAME} ws2_32)
#     # target_link_libraries(${MACHNET_SHIM_LIB_NAME} mswsock)
#     target_link_libraries(${MACHNET_SHIM_LIB_NAME} rpcrt4)
# # else ()
# #     target_link_libraries (${MACHNET_SHIM_LIB_NAME} uuid)
# endif (WIN32)



# # Configure the directories to search for header files.
# target_include_directories(${MACHNET_SHIM_LIB_NAME} PRIVATE .) 

# # Set the version property.
# set_target_properties(${MACHNET_SHIM_LIB_NAME} PROPERTIES VERSION ${PROJECT_VERSION})

# # Set the shared object version property to the project's major version.
# set_target_properties(${MACHNET_SHIM_LIB_NAME} PROPERTIES SOVERSION ${PROJECT_VERSION_MAJOR})
# # set_target_properties(${MACHNET_SHIM_LIB_NAME} PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded$<$<CONFIG:Debug>:DebugDLL>)
# # set_property(TARGET ${MACHNET_SHIM_LIB_NAME} PROPERTY
#     #   MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:DebugDLL")

# # Set the public header property to the one with the actual API.
# set_target_properties(${MACHNET_SHIM_LIB_NAME} PROPERTIES PUBLIC_HEADER machnet.h)
