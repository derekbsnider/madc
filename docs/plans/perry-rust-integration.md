# Perry / Rust Integration Plan

## Status

Exploratory future track. Not on the critical path ahead of `libmadc`
Phase 4, but important enough to shape current API decisions so we do
not create avoidable refactors later.

## Priority Order

The current priority order is:

1. native C++ embedding
2. thin C shim / C ABI
3. Rust and Node.js as later consumers of that stable base

This ordering is intentional:

- madc is implemented in C++
- the first-class embedding surface should be C++ objects with clear
  ownership, diagnostics, IO, and policy behavior
- the C layer should be a thin wrapper over that C++ model
- Rust and Node.js should both bind above a mature host API rather than
  forcing the API shape prematurely

The relative order between Rust and Node.js is intentionally left open.
That decision should be made later based on which integration path
becomes more practical once `libmadc` is stable enough.

## Why Perry Matters

[`Perry`](https://github.com/PerryTS/perry) creates a plausible future
track where JavaScript / TypeScript orchestration can compile to native
code through a Rust-based toolchain. Even if Perry does not become the
primary integration route, it makes Rust more strategically relevant to
the surrounding madc ecosystem:

- Rust may become a strong candidate for service/control-plane code
- Rust FFI ergonomics matter more than they did before
- a clean C++ core plus thin C ABI becomes even more valuable

This does **not** change the immediate order of work. It only means the
current `libmadc` API should stay friendly to eventual Rust wrapping if
that path becomes attractive.

## What This Means For `libmadc` Right Now

Rust integration should influence constraints, not immediate surface
priority.

Current `libmadc` work should therefore preserve these properties:

- no dependence on a C++ ABI as the eventual foreign-language boundary
- structured diagnostics instead of stderr-only reporting
- explicit engine/program ownership and lifecycle
- data-driven configuration and policy objects
- host-controlled IO/policy instead of process-global assumptions
- a thin future C shim that can be wrapped cleanly by Rust

If those are true, later Rust integration should be straightforward.

## Recommended Layering

When Rust integration becomes active, the intended layering should be:

- `libmadc` C++ core/API
  - real implementation
  - primary design surface
- C shim
  - opaque handles
  - thin wrappers over the C++ objects
- `madc-sys`
  - unsafe Rust FFI bindings to the C shim
- `madc`
  - safe Rust wrapper crate

That means Rust should **not** drive the shape of the core engine today.
Rust should consume the already-settled host API.

## Recommended Rust Crate Split

When the time comes, a reasonable crate split is:

```text
madc/
├── madc-sys/
└── madc/
```

- `madc-sys`
  - raw `extern "C"` bindings
  - opaque handle types
  - minimal unsafe layer
- `madc`
  - safe wrapper
  - Rust-native error mapping
  - Rust-native ownership and RAII
  - optional serde-based value conversion

## Example Future Rust Shape

The eventual Rust usage should probably feel roughly like:

```rust
use madc::Engine;

fn main() -> Result<(), madc::Error> {
    let mut engine = Engine::new();
    let mut program = engine.create_program();

    program.load_file("script.mad")?;
    let result = program.call("main")?;

    println!("{result:?}");
    Ok(())
}
```

This is intentionally aligned with the current C++ direction:

- engine object
- program object
- structured errors
- explicit lifecycle

## What Not To Do Yet

Do not do these now:

- no Rust-specific build targets in the main tree
- no `madc-sys` crate yet
- no Perry-specific runtime integration yet
- no JSON-only data model decisions made solely for Rust convenience
- no flattening of the C++ API around FFI before the host model is ready

Those would all be premature.

## Likely Rust-Friendly Surface Requirements

The current Phase 4 work should stay compatible with future Rust needs:

- stable opaque engine/program handles in the eventual C layer
- structured diagnostics retrievable without parsing stderr
- clear compile/load/call result contracts
- controllable policy/config loading
- controllable output/error streams
- a value model that can later map to Rust enums/structs cleanly

## Data Exchange Direction

The original scratch note leaned heavily on JSON for inputs/outputs.
That may still be useful, but it should **not** be the only plan.

Preferred direction:

- first design the native C++ value model
- then decide what the C shim exposes
- only then decide whether Rust wants:
  - native tagged values
  - JSON convenience helpers
  - both

JSON may be a convenience layer, not the foundational ABI.

## Execution Modes

Rust should eventually be able to sit on top of the same execution modes
the rest of `libmadc` uses:

- direct/in-process trusted embedding
- worker/forked restricted execution

This again argues for finishing the core `MadcEngine` / `Program` /
policy model before any Rust-specific code appears.

## Interaction With Node.js

Rust and Perry do not replace the Node.js story yet. They only create a
possible future control-plane option.

Near-term order remains:

1. finish `libmadc` host API
2. finish policy/diagnostics/IO design
3. add C shim
4. evaluate Rust/Perry and Node as alternative or complementary
   consumers on top of that stable base

## Current Impact On Phase 4

This plan affects the current trajectory in a few concrete ways:

- keep the C++ API primary
- keep the future C ABI thin
- avoid process-global behavior in host-facing code
- avoid stderr-only diagnostics contracts
- avoid making JSON the only possible foreign-language data interface

Beyond those constraints, this document should not change the active
implementation order.

## Activation Point

This track should become active only after:

- `MadcEngine` / `Program` lifecycle is stable
- diagnostics are structured
- policy/config model is in place
- basic C shim shape is understood

At that point, it becomes reasonable to:

- sketch the C ABI in Rust terms
- prototype `madc-sys`
- evaluate Perry’s role more concretely

## Summary

Rust integration is worth planning now so `libmadc` stays friendly to it,
but it should not outrank the current C++-first embedding work.

The correct order remains:

- C++ host API first
- C shim second
- Rust and Node.js after that, in whichever order proves most practical
- Perry may influence that later choice, but should not distort the
  current C++-first trajectory
