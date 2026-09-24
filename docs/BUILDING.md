# Building & Deploying Sapo Engine

The engine builds fully offline: `nlohmann/json`, Catch2 and the [cpr](https://github.com/libcpr/cpr)
HTTP wrapper (upstream 1.10.5, BSD-3) are vendored under `src/third_party/`. Nothing is fetched
from the network at configure or build time.

## Requirements

| Dependency | Version | Notes |
|---|---|---|
| CMake | >= 3.28 | Ubuntu 24.04 ships 3.28.3 — OK. Older distros: `pip install cmake` or the [Kitware APT repo](https://apt.kitware.com/) |
| C++23 compiler | GCC >= 13 or Clang >= 17 | Ubuntu 24.04 default `g++` is 13 — OK. On Ubuntu 22.04: `sudo apt install g++-13` and configure with `-DCMAKE_CXX_COMPILER=g++-13` |
| libcurl dev headers | any recent (>= 7.64) | **Only needed for `-DSAPO_ENABLE_CPR=ON`** |

The HTTP transport is opt-in: without `SAPO_ENABLE_CPR` the engine ships a `NullTransport` and
`http.*` blueprint commands fail loudly instead of silently doing nothing.

## Build options

| Option | Default | Effect |
|---|---|---|
| `SAPO_ENABLE_CPR` | `OFF` | Compiles the vendored cpr/libcurl HTTP transport. Needs libcurl dev headers. Off ⇒ `NullTransport`, and `http.*` nodes fail loudly. |
| `SAPO_ENABLE_REDIS` | `OFF` | Compiles `src/redis/SocketRedisClient.cpp`, the POSIX-socket RESP2 client behind `RedisStateStore`. **No external library** — the client is written against Berkeley sockets, so the build stays offline and dependency-free. Needs Linux/macOS/*BSD. Off ⇒ `RedisStateStore` still compiles, and you inject your own `IRedisClient` (hiredis, redis-plus-plus). |
| `SAPO_BUILD_TESTS` | `ON` | Catch2 suite, one ctest case per `tests/test_*.cpp`. |
| `SAPO_WARNINGS_AS_ERRORS` | `OFF` | `-Werror`. Worth turning on in CI. |
| `SAPO_SANITIZERS` | `""` | Comma-separated, e.g. `-DSAPO_SANITIZERS=address,undefined`. |

A production engine that talks to a payment gateway and shares sessions across a fleet:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DSAPO_ENABLE_CPR=ON -DSAPO_ENABLE_REDIS=ON -DSAPO_WARNINGS_AS_ERRORS=ON
```

Session-store configuration (`engine.state_redis` and friends) is documented in
[INTEGRATING.md §4](INTEGRATING.md#4-session-state-in-production).

## Ubuntu quick start

```bash
# one-time: system deps (libcurl4-openssl-dev also pulls in libssl-dev,
# which cpr uses for its TLS/HTTPS support)
sudo apt update
sudo apt install -y g++ cmake libcurl4-openssl-dev

# configure + build (from the repo root)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSAPO_ENABLE_CPR=ON
cmake --build build -j"$(nproc)"

# run the test suite
ctest --test-dir build --output-on-failure

# smoke test
./build/sapoc run examples/hello_world.json --var msisdn=233201234567
```

If libcurl headers are missing, configure fails with the exact `apt`/`dnf`/`brew` line to run.

## Deploying to an Ubuntu server

`sapoc` links dynamically only against libc/libstdc++/libcurl; `libsapo_core.a` and the vendored
cpr are linked in statically. For deployment copy:

- `build/sapoc` — the CLI
- `schemas/` — blueprint JSON schemas (installed to `share/sapo` by `cmake --install`)

```bash
sudo cmake --install build --prefix /opt/sapo    # binary + headers + schemas
/opt/sapo/bin/sapoc version                      # prints "http transport: cpr (libcurl)"
```

On a minimal server only the libcurl runtime is needed (no dev package):
`sudo apt install -y libcurl4`.

## Embedding the engine in your own service (Drogon)

To deploy the DSL inside an existing C++ API instead of running `sapoc` — link
`libsapo_core.a` (or `find_package(SapoEngine)`) and drive
`sapo::runtime::VirtualMachine` from your handlers. Full guide, endpoint design,
state-store and transport options, and a runnable reference service:
see [INTEGRATING.md](INTEGRATING.md) and [`examples/drogon`](../examples/drogon).

## GitHub Actions integration bundle

The `Build Sapo Engine` workflow runs on pushes, pull requests, and manual dispatches. It uses the
Drogon-oriented Release options above, installs the CMake package and headers into a clean
`sapo-dist/` tree, and uploads `sapo-dist.zip` as the
`sapo-engine-drogon-linux-x86_64` workflow artifact. Download it from the run's **Artifacts** section
and extract `sapo-dist.zip` to use the package with a host application. Before uploading, the
workflow smoke-tests `find_package(SapoEngine)` and the `Sapo::core` target. It builds in Ubuntu
24.04 and keeps its generated install tree outside the checked-in `sapo-dist/` snapshot.

## Notes

- **cpr version is pinned by the vendored tree** (`src/third_party/cpr`, 1.10.5), so builds are
  identical on every machine — no Homebrew/apt cpr version drift. To upgrade, replace the tree
  with a newer release and keep `CprTransport.cpp` against the new API.
- **IDE include resolution:** `#include <cpr/cpr.h>` in `src/http/CprTransport.cpp` only resolves
  after a configure with `-DSAPO_ENABLE_CPR=ON`. Point your language server at the generated
  `build/compile_commands.json` (`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`).
- Building cpr without TLS (e.g. constrained images): add `-DCPR_ENABLE_SSL=OFF` — HTTPS calls
  will then fail at the transport layer.
