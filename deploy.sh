#!/bin/bash
set -eu

IMAGE_NAME="gargantua"
PORT=8100
NETWORK="gargantua-net"
DB_NAME="gargantua-db"
STAMP_FILE=".deploy-stamp"

if command -v podman >/dev/null 2>&1; then
    ENGINE="podman"
elif command -v docker >/dev/null 2>&1; then
    ENGINE="docker"
else
    echo "Error: neither podman nor docker is installed." >&2
    exit 1
fi

prop() {
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" app/application.properties \
        | sed 's/[[:space:]]*#.*//; s/[[:space:]]*$//' | tail -1
}

DRIVER=$(prop 'database\.driver')
DRIVER=${DRIVER:-sqlite}

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

echo "Building image with $ENGINE (driver: $DRIVER)..."
"$ENGINE" build -t "$IMAGE_NAME" .

DURATION=$(( $(date +%s) - START_TIME ))
SIZE=$("$ENGINE" images --format '{{.Size}}' "$IMAGE_NAME" 2>/dev/null | head -n 1 || true)

"$ENGINE" rm -f "$IMAGE_NAME" >/dev/null 2>&1 || true

if [ "$DRIVER" = "postgres" ]; then
    # The database is a separate container. Only the backend ships in our image.
    : "${POSTGRES_PASSWORD:=secret}"
    : "${POSTGRES_DB:=gargantua}"
    : "${POSTGRES_USER:=postgres}"

    "$ENGINE" network create "$NETWORK" >/dev/null 2>&1 || true

    if ! "$ENGINE" ps --format '{{.Names}}' | grep -qx "$DB_NAME"; then
        echo "Starting $DB_NAME..."
        "$ENGINE" rm -f "$DB_NAME" >/dev/null 2>&1 || true
        "$ENGINE" run --name "$DB_NAME" --network "$NETWORK" --network-alias db -d \
            -e POSTGRES_PASSWORD="$POSTGRES_PASSWORD" \
            -e POSTGRES_DB="$POSTGRES_DB" \
            -e POSTGRES_USER="$POSTGRES_USER" \
            -v gargantua-pgdata:/var/lib/postgresql/data \
            postgres:16-alpine >/dev/null
        for _ in $(seq 1 30); do
            "$ENGINE" exec "$DB_NAME" pg_isready -U "$POSTGRES_USER" >/dev/null 2>&1 && break
            sleep 1
        done
    fi

    "$ENGINE" run --name "$IMAGE_NAME" --network "$NETWORK" --rm -d \
        -p "$PORT:$PORT" "$IMAGE_NAME" >/dev/null
    DB_LINE="  Database:   $DB_NAME (postgres:16-alpine) on $NETWORK"
else
    mkdir -p data
    "$ENGINE" run --name "$IMAGE_NAME" --rm -d -p "$PORT:$PORT" \
        -v "$(pwd)/data:/data" "$IMAGE_NAME" >/dev/null
    DB_LINE="  Database:   sqlite at ./data"
fi

echo "$SUM" > "$STAMP_FILE"

echo "------------------------------------"
echo "  Gargantua ($ENGINE)"
echo "  Build time: ${DURATION}s"
echo "  Image size: ${SIZE}"
echo "$DB_LINE"
echo "  Running on http://localhost:$PORT"
echo "------------------------------------"
