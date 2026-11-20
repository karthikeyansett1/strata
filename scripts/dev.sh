#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if (( $# == 0 )); then
    echo "Usage: $0 command [args...]" >&2
    exit 2
fi

if [[ $(uname -s) == Linux ]]; then
    cd "$root"
    exec "$@"
fi

image=strata-dev:local
docker build --tag "$image" "$root"
exec docker run --rm --mount "type=bind,source=$root,target=/workspace" "$image" "$@"
