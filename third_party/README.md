# Pinned C dependencies

The I/O release build vendors these upstream releases as git submodules:

| Project | Upstream tag | Commit |
|---|---|---|
| Mbed TLS | `mbedtls-3.6.7` | `068ff080b369adfac81509f9b57b2afabaf82dc5` |
| NNG | `v1.12.5` | `437a5f746722dee04020dc8869894c1f1f02b177` |
| cJSON | `v1.7.19` | `c859b25da02955fef659d658b8f324b5cde87be3` |

Mbed TLS 3.6.7 declares its `framework` submodule; the release's gitlink pins
it to `dde0c4a0e448a0552f18817dcea633bb851fd288`. Initialize dependencies recursively:

```sh
git submodule update --init --recursive
```

The root build selects static, PIC libraries with TLS enabled in NNG and the
in-tree Mbed TLS engine. See the top-level `SOLARI_VENDORED_DEPS` option for
the explicit system-library fallback mode.
