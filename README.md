# storax-gcc-oracle2

A pure-**C++23** rewrite of the g++ 16.1 compile oracle (reflection +
contracts). No interpreter, no third-party libraries, hand-rolled JSON,
**compile-time REST routing** (consteval FNV-1a → `uint64` switch).

Two images, one source:
- **`-full`** (`Dockerfile.full`, ~316 MB): stripped Debian runtime, the
  COMPLETE header set (all x64 + kernel uapi) — any C++26 the model emits
  compiles. Headers installed natively at runtime so every multiarch/uapi
  symlink resolves (a cross-stage COPY of `/usr/include` silently dangles
  `asm/errno.h -> /usr/lib/linux/uapi/...`).
- **`-light`** (`Dockerfile.light`, distroless): minimal, still trimming.

## What the rewrite bought (measured, virre)

| | oracle1 (python) | oracle2-full (c++) |
| --- | --- | --- |
| **image** | **436 MB** | **316 MB** (-120 MB) |
| runtime deps | python3 + stdlib + http.server | none beyond glibc |
| trivial compile | 20.5 ms | 23.8 ms |
| reflection compile | 185 ms | 193 ms |

The win is **size and dependency surface**: -120 MB and Python entirely
gone. The judge of C++ is now itself C++ - one static binary, no
interpreter in the training-critical path. `-light` (distroless, trimming
the copied `/usr/lib` to what cc1plus/as/ld actually need) targets
~150-200 MB, roughly half the original.

**Speed is at parity, not the point.** The few-ms differences are within
LAN + fork noise; the server language was never the bottleneck (both fork
the same g++; cc1plus dominates - raw floor 3 ms trivial, 162 ms
reflection). The one real speed lever, if ever needed, is a `<meta>`
precompiled header baked into the image - caching the 162 ms
reflection-header parse - not another rewrite.

## API (identical to oracle1's core)

    GET  /health   -> {ok, version, reflection, jobs_done}
    POST /compile  {files:{name:src}, args:[...], main, run, timeout}

---
Built for the storax platform. AI-assisted (Claude); design + commits by
the human maintainer.
