---
name: Bug report
about: Crash, hang, wrong result, or behaviour that diverges from the README
title: ""
labels: bug
---

## What happened

A short description of the observed behaviour.

## What you expected

What the docs / type signature / contract led you to expect.

## Reproduction

A minimal source snippet, or a link to a fork with a failing test. The smaller the repro, the faster the fix.

```cpp
// minimal reproduction
```

## Environment

- citor version (`git rev-parse HEAD` or `v` tag):
- Compiler + version (`g++ --version`, `clang++ --version`, `cl`):
- OS + kernel (`uname -a` on Linux, `winver` on Windows):
- CPU model (`lscpu | grep "Model name"` on Linux):
- CMake configure line you used:

## Notes

Anything else worth flagging: TSan output, ASan stack, sysfs oddity, NUMA layout, etc.
