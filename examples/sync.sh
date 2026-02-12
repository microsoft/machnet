#!/usr/bin/env bash
# Sync the machnet workspace to the remote machine, preserving the same path.

REMOTE_USER="sarsanaee"
REMOTE_HOST="asas-westus2-vm-0"
LOCAL_PATH="/home/sarsanaee/machnet/"
REMOTE_PATH="${REMOTE_USER}@${REMOTE_HOST}:/home/sarsanaee/machnet/"

rsync -avz --progress \
  --exclude 'build/' \
  --exclude '.git/' \
  --exclude 'third_party/' \
  "$LOCAL_PATH" "$REMOTE_PATH"
