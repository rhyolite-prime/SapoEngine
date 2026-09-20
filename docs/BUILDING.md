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

## Notes

- **cpr version is pinned by the vendored tree** (`src/third_party/cpr`, 1.10.5), so builds are
  identical on every machine — no Homebrew/apt cpr version drift. To upgrade, replace the tree
  with a newer release and keep `CprTransport.cpp` against the new API.
- **IDE include resolution:** `#include <cpr/cpr.h>` in `src/http/CprTransport.cpp` only resolves
  after a configure with `-DSAPO_ENABLE_CPR=ON`. Point your language server at the generated
  `build/compile_commands.json` (`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`).
- Building cpr without TLS (e.g. constrained images): add `-DCPR_ENABLE_SSL=OFF` — HTTPS calls
  will then fail at the transport layer.
