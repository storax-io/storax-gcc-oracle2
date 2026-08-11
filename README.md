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

## The honest benchmark (virre, vs the Python oracle1)

| | oracle1 (python) | oracle2 (c++) |
| --- | --- | --- |
| trivial compile | 20.5 ms | 23.8 ms |
| reflection compile | 185 ms | 193 ms |

**The C++ rewrite did NOT make it faster** — the server language was never
the bottleneck. Both fork the same g++; cc1plus doing the work dominates.
Raw compiler floor (no server/network): trivial 3 ms, reflection 162 ms.
Bypassing the g++ driver to invoke cc1plus directly saves only 12% on
reflection.

**Where real speed lives (untested):** a `<meta>` precompiled header baked
into the image and force-included. Most of the 162 ms is re-parsing and
re-instantiating the reflection header on every job; a PCH caches that.
This — not the server language, not driver bypass — is the path to a
meaningful speedup. See `docs/` before investing.

## What oracle2 IS good for regardless of speed

A dependency-free, interpreter-free, minimal artifact: the judge of C++ is
itself C++, in one static binary. If "no Python in the training-critical
path" is the goal, this delivers it. If speed is the goal, build the PCH.

## API (identical to oracle1's core)

    GET  /health   -> {ok, version, reflection, jobs_done}
    POST /compile  {files:{name:src}, args:[...], main, run, timeout}

---
Built for the storax platform. AI-assisted (Claude); design + commits by
the human maintainer.
