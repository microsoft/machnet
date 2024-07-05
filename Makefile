# This makefile is primarily used for building docker containers

SHELL=/bin/bash -e -o pipefail

BUILD_COMMAND=docker buildx bake -f docker-bake.hcl
GET_BUILDX_INFO_COMMAND=$(BUILD_COMMAND) --print
BUILD_TARGETS_COMMAND=xargs $(BUILD_COMMAND)
GET_TARGETS_FOR_ARCH_CMD=python3 $(CURDIR)/dockerfiles/get_targets_for_arch.py

# By default, load into the local docker registry, can be overriden with --push for production builds
BUILD_COMMAND_EXTRA_ARGS=--load


.PHONY: all_containers x86_containers arm_containers

# Users likely want to get containers that work on the current system,
# so that is the default.
default_containers: native_containers

all_containers:
	$(BUILD_COMMAND) $(BUILD_COMMAND_EXTRA_ARGS)

x86_containers:
	$(GET_BUILDX_INFO_COMMAND) | $(GET_TARGETS_FOR_ARCH_CMD) --arch x86 | $(BUILD_TARGETS_COMMAND) $(BUILD_COMMAND_EXTRA_ARGS)

arm_containers: 
	$(GET_BUILDX_INFO_COMMAND) | $(GET_TARGETS_FOR_ARCH_CMD) --arch arm | $(BUILD_TARGETS_COMMAND) $(BUILD_COMMAND_EXTRA_ARGS)

native_containers:
	$(GET_BUILDX_INFO_COMMAND) | $(GET_TARGETS_FOR_ARCH_CMD) --arch native | $(BUILD_TARGETS_COMMAND) $(BUILD_COMMAND_EXTRA_ARGS)