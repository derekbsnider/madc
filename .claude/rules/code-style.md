# Code Style Rules

- C++11 standard (`-std=c++11`)
- Tabs for indentation (existing code uses tabs)
- Header guards use `#ifndef __FILENAME_H` / `#define __FILENAME_H 1` pattern
- `DBG(x)` macro wraps debug-only statements — ONE definition, in `include/datadef.h`; see `.claude/rules/debug.md` for full rules
