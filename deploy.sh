#!/bin/bash
set -eu

IMAGE_NAME="gargantua"
PORT=8100
STAMP_FILE=".deploy-stamp"

if command -v podman >/dev/null 2>&1; then
    ENGINE="podman"
elif command -v docker >/dev/null 2>&1; then
    ENGINE="docker"
else
    echo "Error: neither podman nor docker is installed." >&2
    exit 1
fi

checksum() {
    find eat Dockerfile src include app -type f -print0 \
        | sort -z | xargs -0 shasum | shasum | cut -d ' ' -f 1
}

SUM=$(checksum)
RUNNING=$("$ENGINE" ps --format '{{.Names}}' 2>/dev/null | grep -cx "$IMAGE_NAME" || true)

if [ "${1:-}" != "--force" ] && [ "$RUNNING" = "1" ] \
   && [ -f "$STAMP_FILE" ] && [ "$(cat "$STAMP_FILE")" = "$SUM" ]; then
    echo "No changes since last deploy — already running on http://localhost:$PORT"
    echo "(use ./deploy.sh --force to rebuild anyway)"
    exit 0
fi

START_TIME=$(date +%s)

echo "Building image with $ENGINE..."
"$ENGINE" build -t "$IMAGE_NAME" .

DURATION=$(( $(date +%s) - START_TIME ))
SIZE=$("$ENGINE" images --format '{{.Size}}' "$IMAGE_NAME" 2>/dev/null | head -n 1 || true)

"$ENGINE" rm -f "$IMAGE_NAME" >/dev/null 2>&1 || true

mkdir -p data
"$ENGINE" run --name "$IMAGE_NAME" --rm -d -p "$PORT:$PORT" \
    -v "$(pwd)/data:/data" "$IMAGE_NAME" >/dev/null

echo "$SUM" > "$STAMP_FILE"

echo "------------------------------------"
echo "  Gargantua ($ENGINE)"
echo "  Build time: ${DURATION}s"
echo "  Image size: ${SIZE}"
echo "  Database:   sqlite at ./data"
echo "  Running on http://localhost:$PORT"
echo "------------------------------------"
