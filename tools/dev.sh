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

# Pinned to esp-matter's own recommended ESP-IDF version for reproducible
# builds (esp-matter doesn't support ESP-IDF v6.0.x yet — see CLAUDE.md).
# Override with ESP_MATTER_IMAGE=... to track 'latest' or another tag instead.
IMAGE="${ESP_MATTER_IMAGE:-espressif/esp-matter:release-v1.6_idf_v5.5.4}"

# Mount the repository root at /project inside the container.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "Using image: $IMAGE"
echo "Mounting:    $REPO_ROOT -> /project"

if [ "$#" -eq 0 ]; then
    exec docker run --rm -it -v "$REPO_ROOT":/project -w /project "$IMAGE" /bin/bash
else
    exec docker run --rm -it -v "$REPO_ROOT":/project -w /project "$IMAGE" "$@"
fi
