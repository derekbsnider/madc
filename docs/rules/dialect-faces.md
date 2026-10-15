# Dialect Faces — reasoning

The bare rule is `.claude/rules/dialect-faces.md`.

## Why

madc is both a compiler with its own interactive front ends (`madc -i`, the
LSP, the MCP server) and the runtime its dialect programs run on. madcide —
the IDE — is a dialect program. When a capability lives only on madc's C++
side, the CLI uses it and madcide cannot, so madcide grows its own copy, and
the two drift: the same user action looks different in the two front ends.

The split arc (1A–1D, madc 0.104.0) moved capabilities madcide had built in
dialect code into madc precisely so they would be reusable outside madcide.
madcdis carries many of the types madc uses internally for the same reason:
one definition the engine and the dialect share.

## The incident that wrote this down (2026-10-15)

madcide's REPL showed `:%p` uncoloured; the CLI coloured it.

- The ex core's `outcome` carried `code` — the byte ranges of `shown` that
  are a buffer's text — and the CLI painted those ranges through
  `madc::theme_paint` (`madc_theme.h`).
- The dialect face of the ex core (`outcome_value`, `src/madc_ex.cpp`) was
  written before `code` existed and never carried it. The session's reply
  row (`reply_row`, `src/madc_session_verbs.cpp`) dropped `shown_code` the
  same way, so `%type` and `%pinfo` were plain in madcide too.
- `madc_theme.h` was itself a move from madcide (its header says "madcide's
  own reader moves onto it"), but the second half of the move — madcide
  deleting its reader and calling madc's — never happened, and the painter
  had no dialect face at all.
- The first fix attempt (madcide 37f2d0a) then built a SECOND painter inside
  madcide. The owner's question — "the CLI already does this, why does the
  IDE operate so differently?" — is the rule.

## Why a registry and a gate

The failure is silent drift between two lists nobody compares: a struct's
members and the keys its face writes; a header's publics and the dialect's
declarations. A rule that says "keep them in step" decays; a gate that
compares them does not. The registry also makes the omissions visible: a
member a host has no use for is omitted WITH its reason, in the open.

The gate is two-sided: its negative controls prove each reader still finds
members, face keys and publics in a known text, and a registry with a member
removed goes red (verified when the gate was written).

## Scope today

The registry starts with the surfaces of the incident (`ex::outcome`,
`SessionClient::Reply`, `madc_theme.h`). Every face written from now on is
registered; the 0.104.0 split-arc moves are being audited and join the
registry as each one's madcide copy is deleted.
