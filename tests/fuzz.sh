#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
SECONDS_PER_TARGET=${1:-10}
[[ "$SECONDS_PER_TARGET" =~ ^[0-9]+$ ]] || exit 2
((SECONDS_PER_TARGET >= 1 && SECONDS_PER_TARGET <= 600)) || exit 2
FUZZ_CC=${CC:-clang}
FUZZ_OUT=$(mktemp -d .build-fuzz.XXXXXXXX)
trap 'rm -rf -- "$FUZZ_OUT"' EXIT
FUZZ_FLAGS=(-fsanitize=fuzzer,address,undefined)
FUZZ_DRIVER=()
if ! printf 'int LLVMFuzzerTestOneInput(const unsigned char *p, unsigned long n) { return 0; }' |
    "$FUZZ_CC" -x c - -fsanitize=fuzzer -o "$FUZZ_OUT/probe" 2>/dev/null; then
    if [[ "${CI:-}" == true ]]; then
        echo "CI requires a Clang distribution with libFuzzer" >&2
        exit 1
    fi
    FUZZ_FLAGS=(-fsanitize=address,undefined)
    FUZZ_DRIVER=(tests/fuzz/smoke.c)
fi
GENERATOR=()
while IFS= read -r source; do GENERATOR+=("$source"); done \
    < <(find src/generator -name '*.c' ! -name generator_main.c | sort)
for target in http json generator; do
    case "$target" in
        http) deps=(src/http/http.c) ;;
        json) deps=(src/http/json.c) ;;
        generator) deps=("${GENERATOR[@]}") ;;
    esac
    "$FUZZ_CC" -std=c11 -O1 -g -pthread -Iinclude -fno-omit-frame-pointer \
        "${FUZZ_FLAGS[@]}" -fno-sanitize-recover=all \
        "tests/fuzz/$target.c" ${FUZZ_DRIVER[@]+"${FUZZ_DRIVER[@]}"} "${deps[@]}" -o "$FUZZ_OUT/$target"
    mkdir "$FUZZ_OUT/corpus-$target"
    cp tests/fuzz_corpus/"$target"/* "$FUZZ_OUT/corpus-$target/"
    if ! "$FUZZ_OUT/$target" "$FUZZ_OUT/corpus-$target" -seed=42 \
        -max_total_time="$SECONDS_PER_TARGET" -max_len=4096 -rss_limit_mb=1024 \
        -artifact_prefix="$FUZZ_OUT/" 2>"$FUZZ_OUT/$target.log"; then
        cat "$FUZZ_OUT/$target.log" >&2
        cp "$FUZZ_OUT"/crash-* "$FUZZ_OUT"/timeout-* "$FUZZ_OUT"/oom-* /tmp/ 2>/dev/null || true
        exit 1
    fi
    tail -1 "$FUZZ_OUT/$target.log"
done
