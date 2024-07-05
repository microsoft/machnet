#!/usr/bin/env python3

import argparse
import platform
import json
import sys

def main():
    parser = argparse.ArgumentParser(
        prog="Get Targets For Arch",
        description="This program filters the output of 'docker buildx bake -f $BAKEFILE --print' to get a list of targets for a specific architecture.",
    )

    arch_choices = [
        "x86",
        "arm",
        "native",
    ]

    parser.add_argument("--arch", type=str, choices=arch_choices)

    args = parser.parse_args()

    arch = None

    if args.arch == "x86":
        arch = "linux/amd64"
    elif args.arch == "arm":
        arch = "linux/arm64"
    elif args.arch == "native":
        machine = str(platform.machine()).lower()
        if machine == "amd64":
            arch = "linux/amd64"
        elif machine == "aarch64":
            arch = "linux/arm64"
        else:
            print(f"Unknown native arch: {machine}", file=sys.stderr)
            sys.exit(1)
    else:
        print(f"Unknown arch argument: {args.arch}", file=sys.stderr)
        sys.exit(1)

    buildx_info = json.load(sys.stdin)
    
    targets_to_build = list(filter_buildx_info(buildx_info, arch))

    if len(targets_to_build) == 0:
        print("No targets to build", out=sys.stderr)
        sys.exit(1)

    for target in targets_to_build:
        print(target)
    

def filter_buildx_info(buildx_info, platform):
    for target, target_info in buildx_info["target"].items():
        if platform in target_info["platforms"]:
            yield target
    

if __name__ == "__main__":
    main()