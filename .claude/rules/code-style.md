# Code Style Rules

- C++11 standard (`-std=c++11`)
- Tabs for indentation (existing code uses tabs)
- Header guards use `#ifndef __FILENAME_H` / `#define __FILENAME_H 1` pattern
- `DBG(x)` macro wraps debug-only statements — defined as `#define DBG(x) do { if(madc_verbose){x;} } while(0)` in every source file; see `.claude/rules/debug.md` for full rules
