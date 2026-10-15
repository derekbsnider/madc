# Dialect Faces — what madc builds for itself is exposed in the dialect

- OWNER LAW (restated 2026-10-15): a capability madc builds — a reader, a
  painter, a classifier, a struct a host reads — is reachable from the
  dialect through a face, so a dialect host (madcide) does what the CLI does
  by calling madc's code. madcdis's shared types are the precedent.
- A host never re-implements a rule madc owns. Before writing dialect logic
  the CLI already performs, find madc's owner and call or expose it.
- A move from a host into madc is done when the host's copy is DELETED and
  the host calls madc's face — not when madc has its copy.
- When an engine struct a face converts gains a member, the face carries it
  in the same change, or the registry omits it with a reason.
- A face is declared in a dialect header (`<ns_madc>`, `<ns_madc_host>`,
  `<bits/...>`) and resolves mangled-direct (`cpp-first-api.md`); a host-only
  face also gets its auto-include member row (src/lexer.cpp).
- Register every face in `scripts/dialect_faces.tsv`: a struct row (member ↔
  key) or a public row (function ↔ face, or `exempt:` with the reason).
- Gate: `scripts/check-dialect-faces.sh` (`make gates`) — red on a member with
  no key, a public with no face or exempt reason, a face the dialect header
  does not declare; two-sided negative controls.

See `docs/rules/dialect-faces.md` for the reasoning.
