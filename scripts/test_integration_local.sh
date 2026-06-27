#!/usr/bin/env bash
# Run integration tests locally against the same postgres:17 + redis:7 images used in CI.
# Usage: bash scripts/test_integration_local.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BINARY="$ROOT/build-ci/tests/wikore_tests"
NET="wikore-test-net"
PG_CONTAINER="wikore-test-pg"
REDIS_CONTAINER="wikore-test-redis"
PG_PASSWORD="wikore"

cleanup() {
    echo "-- Stopping containers..."
    docker rm -f "$PG_CONTAINER" "$REDIS_CONTAINER" 2>/dev/null || true
    docker network rm "$NET" 2>/dev/null || true
}
trap cleanup EXIT

if [[ ! -x "$BINARY" ]]; then
    echo "ERROR: $BINARY not found. Run: cmake --build build-ci --parallel" >&2
    exit 1
fi

echo "-- Creating network $NET..."
docker network create "$NET" 2>/dev/null || true

echo "-- Starting postgres:17..."
docker run -d --name "$PG_CONTAINER" --network "$NET" \
    -e POSTGRES_USER=wikore \
    -e POSTGRES_PASSWORD="$PG_PASSWORD" \
    -e POSTGRES_DB=wikore \
    -p 5433:5432 \
    postgres:17

echo "-- Starting redis:7..."
docker run -d --name "$REDIS_CONTAINER" --network "$NET" \
    -p 6380:6379 \
    redis:7

echo "-- Waiting for postgres..."
until docker exec "$PG_CONTAINER" pg_isready -U wikore -q; do sleep 1; done

echo "-- Loading migrations..."
cat "$ROOT"/db/migrations/V001__orgs.sql \
    "$ROOT"/db/migrations/V002__users_and_auth.sql \
    "$ROOT"/db/migrations/V003__documents.sql \
    "$ROOT"/db/migrations/V004__wiki.sql \
    "$ROOT"/db/migrations/V005__chat.sql \
    "$ROOT"/db/migrations/V006__integrations.sql \
    "$ROOT"/db/migrations/V007__audit.sql \
    "$ROOT"/db/migrations/V009__grant_validation.sql \
    "$ROOT"/db/migrations/V010__promotion_functions.sql \
    "$ROOT"/db/migrations/V011__deactivation_expiry_audit.sql \
    "$ROOT"/db/migrations/V012__move_org_unit.sql \
    "$ROOT"/db/migrations/V013__reactivate_user.sql \
    "$ROOT"/db/migrations/V014__sensitivity_sections_tombstones.sql \
    "$ROOT"/db/migrations/V015__outbox.sql \
  | PGPASSWORD="$PG_PASSWORD" psql -h localhost -p 5433 -U wikore -d wikore -v ON_ERROR_STOP=1 -q

echo "-- Running integration tests..."
DATABASE_URL="postgresql://wikore:${PG_PASSWORD}@localhost:5433/wikore" \
REDIS_URL="redis://localhost:6380/0" \
    "$BINARY" "[integration]"
