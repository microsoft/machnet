#!/bin/bash
# Build a container image Machnet service and push it to ghcr
#
# Assumptions:
#  - ${GITHUB_PAT} contains the user's Github personal access token
#  - User has access to push to msr-machnet
# Usage: dockerbuild.sh

echo "Building Machnet Docker image"
git submodule update --init --recursive

# Fill-up the Image folder
mkdir -p Image
rm -rf Image/*
mkdir -p Image/machnet

for f in src third_party CMakeLists.txt servers.json; do
    echo "Copying $f to Image/machnet"
    cp -r ../$f Image/machnet/$f
done

echo "Running Docker build"
sudo docker build . -t ghcr.io/msr-machnet/machnet:latest -f Dockerfile --build-arg timezone="$(cat /etc/timezone)"

echo "Logging into ghcr, assuming Github username = $USER"
if [ -z "$GITHUB_PAT" ]; then
    echo "GITHUB_PAT is not set. Please set it to your GitHub Personal Access Token."
    exit 1
else
    echo "GITHUB_PAT is set. Using it to log into ghcr."
fi

echo ${GITHUB_PAT} | sudo docker login ghcr.io -u $USER --password-stdin
sudo docker push ghcr.io/msr-machnet/machnet:latest
