group "default" {
    targets = [
        "machnet-ubuntu",
        "machnet-amazon-linux",
    ]
}

arm_list = [for dpdk_platform in [
    {
        label = "generic"
        cflags = "",
    }, {
        label = "graviton2"
        cflags = "-mcpu=neoverse-n1"
    }, {
        label = "graviton3",
        cflags = "-mcpu=neoverse-v1",
    }] : {
    dpdk_platform = "${dpdk_platform.label}",
    platform = "linux/arm64",
    cpu_instruction_set = null,
    cflags = "${dpdk_platform.cflags}",
    label = "arm-${dpdk_platform.label}",
}]
x86_list = [for dpdk_platform in ["x86-64-v2", "x86-64-v3", "x86-64-v4"] : {
    dpdk_platform = "native",
    platform = "linux/amd64",
    cpu_instruction_set = "${dpdk_platform}",
    cflags = "-march=${dpdk_platform}",
    label = "${dpdk_platform}",
}]

configs = setunion(arm_list, x86_list)

target "machnet-amazon-linux" {
    name = "machnet-amazon-linux-${item.label}"
    dockerfile = "dockerfiles/amazon-linux-2023.dockerfile"
    platforms = ["${item.platform}"]
    tags = [
        "ghcr.io/microsoft/machnet/machnet:${item.label}", "ghcr.io/microsoft/machnet/machnet:amazon-linux-${item.label}",
        "ghcr.io/microsoft/machnet/machnet:amazon-linux-2023-${item.label}", "machnet:${item.label}",
    ]
    matrix = {
        item = configs
    }
    args = {
        DPDK_PLATFORM = "${item.dpdk_platform}",
        DPDK_EXTRA_MESON_DEFINES = join(" ", ["-Dmax_numa_nodes=1 -Ddefault_library=static", (item.cpu_instruction_set != null ? " -Dcpu_instruction_set=${item.cpu_instruction_set}" : "")]),
        CFLAGS = item.cflags
        CXXFLAGS = item.cflags
        timezone = "America/New_York"
    }
}

target "machnet-ubuntu" {
    name = "machnet-ubuntu-${item.label}"
    dockerfile = "dockerfiles/ubuntu-22.04.dockerfile"
    tags = [
        "ghcr.io/microsoft/machnet/machnet:${item.label}", "ghcr.io/microsoft/machnet/machnet:ubuntu-${item.label}", "ghcr.io/microsoft/machnet/machnet:ubuntu-22.04-${item.label}"
    ]
    platforms = ["${item.platform}"]
    matrix = {
        item = configs
    }
    args = {
        DPDK_PLATFORM = "${item.dpdk_platform}",
        DPDK_EXTRA_MESON_DEFINES = join(" ", ["-Dmax_numa_nodes=1 -Ddefault_library=static", (item.cpu_instruction_set != null ? " -Dcpu_instruction_set=${item.cpu_instruction_set}" : "")]),
        CFLAGS = item.cflags
        CXXFLAGS = item.cflags
        timezone = "America/New_York"
    }
}
