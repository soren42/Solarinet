# Unity (vendored)

This directory is the vendoring slot for **Unity** — the tiny pure-C unit-test
framework (ThrowTheSwitch/Unity, MIT) mandated by §3 / §14 of the architecture
plan.

## Current contents

`unity.h` + `unity.c` here are a **minimal, API-compatible subset** of upstream
Unity, written so the test suites under `tests/unit/` compile and run with zero
network access during initial development. The macros used by the suites
(`UNITY_BEGIN`, `RUN_TEST`, `TEST_ASSERT_*`, `setUp`/`tearDown`, `UNITY_END`)
behave identically to upstream.

## To drop in the real upstream Unity

Replace `unity.h`, `unity.c`, and add `unity_internals.h` from a pinned release:

```
git submodule add https://github.com/ThrowTheSwitch/Unity third_party/unity
# or copy src/unity.{c,h} and src/unity_internals.h from a release tarball
```

The test sources need no changes — they use only the common Unity surface.
