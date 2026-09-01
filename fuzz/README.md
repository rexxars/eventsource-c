# Fuzzing

Requires clang.

    cmake -B build-fuzz -DSSE_BUILD_FUZZERS=ON -DSSE_BUILD_TESTS=OFF -DCMAKE_C_COMPILER=clang
    cmake --build build-fuzz
    ./build-fuzz/fuzz_parser -max_total_time=60

Any crash or sanitizer report is a bug in the parser; minimize with
`./build-fuzz/fuzz_parser <crash-file> -minimize_crash=1`.

## Building without libFuzzer

On toolchains without libFuzzer (e.g. Apple clang), build and run the standalone smoke driver:

    cmake -B build -DSSE_BUILD_FUZZ_SMOKE=ON && cmake --build build
    ./build/fuzz_smoke

This runs a deterministic pseudo-random corpus with UBSan enabled. To replay a crash file from CI, pass it as an argument:

    ./build/fuzz_smoke <crash-file>
