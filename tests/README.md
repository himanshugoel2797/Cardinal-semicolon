# Host unit tests

Small, dependency-light tests for the parts of the tree that are freestanding in
the kernel but still compile and run on the host. They need neither the cross
toolchain nor QEMU, so they are cheap to run on every change and in CI.

```bash
./scripts/run-tests.sh        # configure tests/, build, run via ctest
```

## What is covered

- **`test_sha256.c`** — SHA-256 (`libs/crypto/sha256.c`) against FIPS 180-2
  vectors plus an incremental-vs-one-shot check. This is the hash beneath the
  module-signing MAC.
- **`test_hmac.c`** — the module-signing HMAC (`libs/crypto/hmac.c`).
  **Note:** this is a non-standard construction (see the file header and
  `notes/AUDIT.md`), so the tests pin determinism and key/message sensitivity
  rather than RFC vectors.

## Adding a test

Drop a `test_<thing>.c` in this directory that defines a `void test_<thing>(void)`
using the `CHECK` / `CHECK_EQ_U` macros from `test_framework.h`, declare it in
that header, and call it from `test_main.c`. Sources are globbed, so re-run
`run-tests.sh` (which re-configures). Keep tests free of kernel dependencies;
link real library sources via the `tests/CMakeLists.txt` rather than copying code.
