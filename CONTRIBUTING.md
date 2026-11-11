# Contributing

## Build and test

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

Before opening a pull request, also run the sanitizer presets for any change that touches
memory layout or threading:

```sh
cmake --preset asan && cmake --build --preset asan --parallel && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan --parallel && ctest --preset tsan
```

With Homebrew LLVM on macOS, the sanitizer build must also link Homebrew's libc++:

```sh
cmake --preset asan \
    -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++ \
    -DCMAKE_EXE_LINKER_FLAGS="-L$(brew --prefix llvm)/lib/c++ -Wl,-rpath,$(brew --prefix llvm)/lib/c++"
```

## Ground rules

- **No new dependencies** in the engine, CLI or tests. Third-party code is allowed only in
  `benchmarks/market/`, where it is the thing being measured.
- **Dependencies point one way.** Algorithm libraries (`aether_hnsw`, `aether_lexical`,
  `aether_hybrid`, ...) do not include application or CLI headers. The CLI contains
  parsing and presentation only.
- **Zero warnings.** CI builds with `-Wall -Wextra -Wpedantic -Werror`.
- **Format with `clang-format`** using the repository's `.clang-format`.
- **Every behaviour change needs a test.** Unit tests live in `tests/unit`, cross-module
  tests in `tests/integration`, and CLI golden-file tests in `tests/e2e`. Register new
  tests in `tests/CMakeLists.txt`.
- **Pruned and exhaustive paths must agree bit for bit.** A new BM25 strategy or HNSW
  shortcut has to be added to the equivalence tests.

## Performance changes

State the workload, the before and after numbers, and the machine. Performance thresholds
are opt-in (`-DAETHER_REGISTER_PERFORMANCE_TESTS=ON`) because they depend on the host, so
they do not run in CI.
