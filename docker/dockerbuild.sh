#!/bin/bash
# Build a container image NSaaS service and push it to ghcr
#
# Assumptions:
#  - ${GITHUB_PAT} contains the user's Github personal access token
#  - User has access to push to msr-nsaas
# Usage: dockerbuild.sh

echo "Building NSaaS Docker image"
git submodule update --init --recursive

# Fill-up the Image folder
mkdir -p Image
rm -rf Image/*
mkdir -p Image/nsaas

for f in src third_party CMakeLists.txt servers.json; do
    echo "Copying $f to Image/nsaas"
    cp -r ../$f Image/nsaas/$f
done

echo "Running Docker build"
sudo docker build . -t ghcr.io/msr-nsaas/nsaas:latest -f Dockerfile --build-arg timezone="$(cat /etc/timezone)"

echo "Logging into ghcr, assuming Github username = $USER"
if [ -z "$GITHUB_PAT" ]; then
    echo "GITHUB_PAT is not set. Please set it to your GitHub Personal Access Token."
    exit 1
else
    echo "GITHUB_PAT is set. Using it to log into ghcr."
fi

echo ${GITHUB_PAT} | sudo docker login ghcr.io -u $USER --password-stdin
sudo docker push ghcr.io/msr-nsaas/nsaas:latest
