# Thread Safety — every language addition states and honors its contract

- OWNER LAW (2026-08-20): anything added to the madc language surface —
  intrinsics, namespace functions, runtime helpers, containers, carriers —
  must be thread-safe.
- "Thread-safe" means a STATED contract, not blanket internal locking. The
  default contract is the C++ standard-library convention: concurrent reads
  are safe, operations on DISTINCT objects are safe, shared mutation
  requires synchronization.
- Shared-mutation designs route through the hub/verb/channel machinery
  (docs/plans/2026-08-20-data-hub-projection-rendering.md, demand 15) —
  never ad-hoc locks scattered through runtime helpers.
- Every new feature's plan or design doc states its thread-safety contract
  explicitly. A feature with an unstated contract is not done.
- New runtime state is per-context, immutable, or atomic. No new bare
  mutable globals.
- The pre-law surface is AUDIT DEBT owned by the F2 (programs-use-cores)
  arc — never silently claimed safe.
- Gate: when F2 lands language threads, a TSan lane joins the battery;
  until then the gate is contract review at design time.

See `docs/rules/thread-safety.md` for the reasoning.
