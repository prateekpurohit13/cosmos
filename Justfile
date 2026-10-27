# Justfile for Cosmos
set shell := ["bash", "-uc"]

# Each devShell sets its own BUILD_DIR so toolchains never share a cmake cache.
build_dir := env_var_or_default("BUILD_DIR", "build")
rkv_artifacts := build_dir + "/ui-artifacts/replicated-kv"

default:
    @just --list

# Build production binaries
build:
    cmake -B {{build_dir}} -DCMAKE_BUILD_TYPE=Release
    cmake --build {{build_dir}} --parallel --target kv_store_prod replicated_kv_prod

# Build simulation binaries only
sim:
    cmake -B {{build_dir}} -DCOSMOS_BUILD_TESTS=OFF
    cmake --build {{build_dir}} --parallel --target kv_store_sim replicated_kv_sim

# Build and run unit tests
test:
    cmake -B {{build_dir}} -DCOSMOS_BUILD_TESTS=ON
    cmake --build {{build_dir}} --parallel
    ctest --test-dir {{build_dir}} --output-on-failure
    @just ui-test

# Run the local UI server contract tests (Python stdlib, no browser required)
ui-test:
    python3 -m unittest discover -s ui/server -p 'test_*.py' -v

# Start the read-only UI server on loopback against an artifact root
ui root="ui/fixtures" port="8000":
    python3 ui/server/serve.py --root {{root}} --port {{port}}

# Build all targets (production binaries, simulation examples, and tests)
all:
    cmake -B {{build_dir}} -DCOSMOS_BUILD_TESTS=ON
    cmake --build {{build_dir}} --parallel

# Build the v1 artifact exporter and write a demo campaign directory
ui-export dir="ui/fixtures/demo" id="demo" trials="4":
    cmake -B {{build_dir}} -DCOSMOS_BUILD_TESTS=ON
    cmake --build {{build_dir}} --parallel --target cosmos_ui_export
    {{build_dir}}/tools/cosmos_ui_export {{dir}} {{id}} {{trials}}

# Regenerate the checked-in fixture deterministically (fixed timestamps, zeroed wall time)
ui-fixture:
    just ui-export ui/fixtures/demo demo 4
    python3 tools/normalise_ui_fixture.py ui/fixtures/demo

# Real replicated-KV campaign: swarm-sampled wrapper faults, process/network episodes, real build
# provenance, canonical traces, inspection events, state and minimization reports. Written under
# the build directory so the checked-in demo fixture stays the only campaign the UI tests see.
# Use `--mode regression` for the exact partition+crash repro from tests/test_distributed.cpp.
rkv-campaign *args:
    cmake -B {{build_dir}} -DCOSMOS_BUILD_TESTS=ON
    cmake --build {{build_dir}} --parallel --target replicated_kv_campaign
    commit="$(jj log -r @ --no-graph -T 'commit_id' 2>/dev/null || true)"; \
    dirty=0; if [ -n "$(jj diff --summary 2>/dev/null || true)" ]; then dirty=1; fi; \
    binary_hash="$(sha256sum {{build_dir}}/tools/replicated_kv_campaign | cut -d' ' -f1)"; \
    dirty_flag="--clean"; if [ "$dirty" = "1" ]; then dirty_flag="--dirty"; fi; \
    {{build_dir}}/tools/replicated_kv_campaign \
        --root {{rkv_artifacts}} --id replicated-kv \
        --commit "$commit" --binary-hash "$binary_hash" "$dirty_flag" \
        {{args}}

# Backwards-compatible name for the real campaign target.
ui-rkv *args:
    just rkv-campaign {{args}}

# Format all C/C++ source and header files
format:
    find include src tests examples tools -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.c" -o -name "*.h" \) -exec clang-format -i {} +

# Check code formatting without modifying files
format-check:
    find include src tests examples tools -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.c" -o -name "*.h" \) -exec clang-format --dry-run --Werror {} +

# Lint codebase (alias for format-check)
lint: format-check

# Same suite on the second toolchain: --wrap is linker interposition, so lld parity is a real
# check. This flake's clang has no static libstdc++, so the static twins skip by design.
test-clang:
    nix develop .#clang -c just test

# Remove this shell's build directory
clean:
    rm -rf {{build_dir}}

alias b := build
alias s := sim
alias t := test
alias fmt := format
alias l := lint
alias c := clean
