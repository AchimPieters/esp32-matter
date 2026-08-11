#!/usr/bin/env bash
#
# Start the Docker-based development environment (StudioPieters style, adapted
# for Matter). This drops you into a shell inside the espressif/esp-matter
# image with ESP-IDF *and* esp-matter already installed and exported.
#
# Everything runs locally on your machine. Nothing is uploaded anywhere.
#
# Usage:
#   ./tools/dev.sh            # open an interactive shell in the container
#   ./tools/dev.sh <command>  # run a single command in the container
#
# Prerequisite: Docker Desktop installed and running.

set -euo pipefail

# Pin the image tag deliberately. 'latest' tracks the newest esp-matter release;
# pin to e.g. release-v1.5 for reproducible builds.
IMAGE="${ESP_MATTER_IMAGE:-espressif/esp-matter:latest}"

# Mount the repository root at /project inside the container.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "Using image: $IMAGE"
echo "Mounting:    $REPO_ROOT -> /project"

if [ "$#" -eq 0 ]; then
    exec docker run --rm -it -v "$REPO_ROOT":/project -w /project "$IMAGE" /bin/bash
else
    exec docker run --rm -it -v "$REPO_ROOT":/project -w /project "$IMAGE" "$@"
fi
