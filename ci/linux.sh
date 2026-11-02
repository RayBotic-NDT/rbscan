#!/usr/bin/env bash
# Linux CI steps, also runnable on a developer machine (or WSL):
#
#   ci/linux.sh build     configure and build (Release) into build-linux/
#   ci/linux.sh test      ctest incl. pytest, JUnit report in build-linux/
#   ci/linux.sh package   SDK tarball, SDK consumer test, wheels and sdist in dist/
#   ci/linux.sh publish   upload dist/ to the GitLab Generic Package Registry
#
# Needs a C++20 compiler, git and curl. uv (pinned), Python, CMake and Ninja are
# provisioned into .deps/ and .venv/.
#
# Inside a manylinux image (quay.io/pypa/manylinux_2_28_x86_64, which sets
# AUDITWHEEL_PLAT) the wheels are repaired with auditwheel and get a manylinux
# tag, as PyPI requires. Elsewhere they stay linux_x86_64 (not for PyPI).
set -euo pipefail
cd "$(dirname "$0")/.."

UV_VERSION="${UV_VERSION:-0.12.5}"
BUILD_DIR="${BUILD_DIR:-build-linux}"
WHEEL_PYTHONS="${RBSCAN_WHEEL_PYTHONS:-3.11 3.12 3.13 3.14}"

export UV_PYTHON_INSTALL_DIR="$PWD/.deps/uv-python"
export UV_CACHE_DIR="$PWD/.deps/uv-cache"
# A distribution's Python often lacks the headers; uv's own builds have them.
export UV_PYTHON_PREFERENCE=only-managed

# HDF5 and the build trees depend on the compiler and glibc of the image, so
# each platform gets its own.
PLATFORM="${AUDITWHEEL_PLAT:-host}"
export RBSCAN_DEPS_DIR="$PWD/.deps/$PLATFORM"

setup_python() {
    if [ "$(.deps/uv/uv --version 2>/dev/null | cut -d' ' -f2)" != "$UV_VERSION" ]; then
        echo "Installing uv $UV_VERSION into .deps/uv"
        curl -LsSf "https://astral.sh/uv/$UV_VERSION/install.sh" |
            env UV_INSTALL_DIR="$PWD/.deps/uv" UV_NO_MODIFY_PATH=1 sh
    fi
    export PATH="$PWD/.deps/uv:$PATH"
    [ -x .venv/bin/python ] || uv venv --python 3.12 .venv
    uv pip install --python .venv/bin/python cmake ninja "pybind11>=3.0" "numpy>=1.24" \
        "pytest>=8" "h5py>=3.10" "hdf5plugin>=4.4"
    export PATH="$PWD/.venv/bin:$PATH"
}

# What a build tree depends on besides the sources. CI workspaces outlive jobs
# and images, so a tree is only reused when this matches.
toolchain_id() {
    local cxx
    cxx=$(command -v "${CXX:-c++}" || echo none)
    echo "$PLATFORM $cxx $("$cxx" -dumpfullversion 2>/dev/null) $(ldd --version 2>&1 | head -n 1)"
}

configure() {
    local id cached_cxx
    id=$(toolchain_id)
    cached_cxx=$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null)
    if [ -f "$BUILD_DIR/CMakeCache.txt" ] &&
        { [ "$(cat "$BUILD_DIR/.toolchain" 2>/dev/null)" != "$id" ] ||
            [ ! -x "$cached_cxx" ]; }; then
        echo "$BUILD_DIR was built with another toolchain; starting over"
        rm -rf "$BUILD_DIR"
    fi
    mkdir -p "$BUILD_DIR"
    echo "$id" >"$BUILD_DIR/.toolchain"
    cmake -S . -B "$BUILD_DIR" -G "Ninja Multi-Config" \
        -DPython_EXECUTABLE="$PWD/.venv/bin/python" \
        -DRBSCAN_BUILD_TESTS=ON -DRBSCAN_BUILD_PYTHON=ON \
        -DRBSCAN_BUILD_EXAMPLES=ON -DRBSCAN_BUILD_TOOLS=ON \
        -DRBSCAN_WARNINGS_AS_ERRORS=ON
}

# Configure (which also drops a build tree of another platform) and build
# incrementally. test and package call it too: a CI job may find a build tree
# from an older pipeline or another image in its workspace, and binaries built
# against a newer glibc do not even start in the manylinux image.
ensure_built() {
    configure
    cmake --build "$BUILD_DIR" --config Release --parallel
}

build() {
    # Release tags always start from a clean build tree.
    if [ -n "${CI_COMMIT_TAG:-}" ]; then rm -rf "$BUILD_DIR"; fi
    ensure_built
}

run_tests() {
    ensure_built
    ctest --test-dir "$BUILD_DIR" -C Release --output-on-failure \
        --output-junit ctest_report.xml
}

package() {
    ensure_built
    rm -rf dist
    mkdir -p dist/wheels

    cmake --install "$BUILD_DIR" --config Release --component sdk --prefix dist/sdk
    tar -czf dist/rbscan-sdk-linux-x86_64.tar.gz -C dist/sdk .

    echo "Building the SDK consumer test against dist/sdk"
    rm -rf "$BUILD_DIR/consumer"
    cmake -S tests/consumer -B "$BUILD_DIR/consumer" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/dist/sdk"
    cmake --build "$BUILD_DIR/consumer"
    "$BUILD_DIR/consumer/rbscan_consumer" "$BUILD_DIR"
    rm -rf dist/sdk

    # Wheel build trees (build/py-<tag>) have the same name on every platform.
    rm -rf build/py-*
    for py in $WHEEL_PYTHONS; do
        echo "Building wheel for Python $py"
        uv build --wheel --python "$py" -o dist/raw-wheels
    done
    if [ -n "${AUDITWHEEL_PLAT:-}" ]; then
        uv pip install --python .venv/bin/python auditwheel
        for w in dist/raw-wheels/*.whl; do
            auditwheel repair --plat "$AUDITWHEEL_PLAT" -w dist/wheels "$w"
        done
    else
        echo "Not in a manylinux image: wheels keep the linux_x86_64 tag (not for PyPI)"
        mv dist/raw-wheels/*.whl dist/wheels/
    fi
    rm -rf dist/raw-wheels

    uv build --sdist -o dist
    rm -f dist/.gitignore

    wheel=$(ls dist/wheels/*-cp312-*.whl | head -n 1)
    echo "Smoke-testing $wheel"
    uv run --isolated --no-project --python 3.12 --with "$wheel" --with pytest --with h5py \
        --with hdf5plugin pytest python/tests

    ls -l dist dist/wheels
}

publish() {
    local version
    version=$(release_version)
    for file in dist/*.tar.gz dist/wheels/*.whl; do
        url="$CI_API_V4_URL/projects/$CI_PROJECT_ID/packages/generic/rbscan/$version/$(basename "$file")"
        echo "Publishing $file to $url"
        curl --fail --silent --show-error --header "JOB-TOKEN: $CI_JOB_TOKEN" \
            --upload-file "$file" "$url"
        echo
    done
}

# Version from CMakeLists.txt. On a tag it must match the tag (v1.2.3 or 1.2.3);
# elsewhere a dev version per commit.
release_version() {
    local cmake_version
    cmake_version=$(grep -m1 -E '^\s+VERSION [0-9]' CMakeLists.txt | awk '{print $2}')
    if [ -n "${CI_COMMIT_TAG:-}" ]; then
        if [ "${CI_COMMIT_TAG#v}" != "$cmake_version" ]; then
            echo "Tag $CI_COMMIT_TAG does not match the version $cmake_version in CMakeLists.txt" >&2
            exit 1
        fi
        echo "$cmake_version"
    else
        echo "$cmake_version-dev.${CI_COMMIT_SHORT_SHA:-local}"
    fi
}

case "${1:-}" in
build) setup_python && build ;;
test) setup_python && run_tests ;;
package) setup_python && package ;;
publish) publish ;;
version) release_version ;;
*)
    echo "usage: $0 build|test|package|publish|version" >&2
    exit 2
    ;;
esac
