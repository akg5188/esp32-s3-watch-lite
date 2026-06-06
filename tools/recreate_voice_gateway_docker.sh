#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="$ROOT/.env.voice-gateway"
COMPOSE_FILE="$ROOT/docker-compose.voice-gateway.yml"

if ! command -v docker >/dev/null 2>&1; then
    echo "docker is not installed." >&2
    exit 127
fi

cd "$ROOT"

docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" down --remove-orphans
docker rm -f watch-voice-gateway >/dev/null 2>&1 || true
docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" up -d --force-recreate
docker compose --env-file "$ENV_FILE" -f "$COMPOSE_FILE" ps
docker port watch-voice-gateway || true

for _ in $(seq 1 20); do
    if curl -fsS --max-time 2 http://127.0.0.1:8790/health; then
        echo
        echo "voice gateway is reachable on http://127.0.0.1:8790"
        exit 0
    fi
    sleep 1
done

echo
echo "host port is not reachable yet; container logs:" >&2
docker logs --tail=120 watch-voice-gateway >&2 || true
exit 1
