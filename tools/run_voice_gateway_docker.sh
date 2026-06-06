#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="$ROOT/.env.voice-gateway"
COMPOSE_FILE="$ROOT/docker-compose.voice-gateway.yml"

if ! command -v docker >/dev/null 2>&1; then
    echo "docker is not installed. Install Docker first, then run this script again." >&2
    exit 127
fi

if ! docker info >/dev/null 2>&1; then
    echo "docker exists, but this user cannot access the Docker daemon." >&2
    echo "Either run this script with sudo, or add your user to the docker group after understanding that it is root-equivalent." >&2
    exit 1
fi

cd "$ROOT"
docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" up -d --build "$@"
docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" ps
