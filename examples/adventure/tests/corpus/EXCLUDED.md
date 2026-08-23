# Excluded reference logs (13 of 107)

The vendored corpus is every open-adventure `tests/*.log` / `.chk`
pair whose behavior is in scope for the madc port. The gate
(`scripts/adventure_parity.sh`) runs ALL vendored pairs — exclusions
shrink only by removing a file, never by runner logic.

Out of scope, by class:

**Binary save/resume (11)** — the reference's `save`/`resume` verbs
write/read its private binary savefile format; the madc port's save
model is the `.world` round-trip instead.

- saveresume.1, saveresume.2, saveresume.3, saveresume.4
- savefail, resumefail, resumefail2
- badmagic, cheatresume, cheatresume2, savetamper

**Reference-binary CLI flags (2)** — exercise `advent`'s command-line
options, not game semantics:

- saveresumeopt (`-r` resume-from-file option; its own header says
  `#NOCOMPARE`)
- oldstyle (`#options: -o` uppercase-mode)
