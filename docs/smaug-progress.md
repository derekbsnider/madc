# SMAUG 1.8 on madc — Progress

Running estimate of how close madc is to executing the SMAUG 1.8
codebase end-to-end. Updated each time a front edge advances or a
language gap closes.

The SMAUG sources are compiled directly by madc from the original
upstream C — this is not a port. The
[MadSMAUG](https://github.com/derekbsnider/MadSMAUG) repo contains
the bootstrap shim (`SMAUG.mad`) and a symlink to the upstream sources;
madc lands the language/compiler work needed to handle real-world C.

Upstream total: **158,537 lines** across
`MadSMAUG/upstream/smaug1.8/src/*.{c,h}` (including IMC sources, which
are guarded under `#ifdef IMC`).

## Status snapshot — 2026-05-20 (milestone record)

**SMAUG runs end-to-end on madc** — both JIT and as a native executable.
(That remains true; this document is the milestone record. Ongoing SMAUG
work is tracked in the MadSMAUG repo.)

| Phase       |   % | Notes |
|-------------|----:|-------|
| **Parse**   | ~86% | 136k of 158k lines ingested. 52 upstream C source files + 9 headers. |
| **Compile** | ~86% | Every ingested TU compiles cleanly. |
| **Link**    | ~95% | All 1878 user-defined functions bind labels. |
| **Runtime** | ~99% | JIT and native EXE both survive login, character creation, room navigation, and combat. The standalone `smaug.exe` survives repeated combat rounds and completes the Newgate room 109 serpent fight. |

## Files compiled by `SMAUG.mad`

### Headers (9)

`mud.h`, `bet.h`, `hint.h`, `house.h`, `news.h`, `imc-mercdefs.h`,
`imc-mercbase.h`, `imc-config.h`, `imc.h`

### C sources (52)

`act_move.c`, `db.c`, `hashstr.c`, `handler.c`, `fight.c`, `skills.c`,
`news.c`, `magic.c`, `mud_prog.c`, `stances.c`, `requests.c`,
`act_comm.c`, `act_obj.c`, `boards.c`, `act_info.c`, `act_wiz.c`,
`ban.c`, `comments.c`, `const.c`, `clans.c`, `colorize.c`, `deity.c`,
`hint.c`, `grub.c`, `comm.c`, `tables.c`, `save.c`, `misc.c`,
`reset.c`, `mapout.c`, `special.c`, `makeobjs.c`, `variables.c`,
`update.c`, `imm_host.c`, `polymorph.c`, `planes.c`, `house.c`,
`starmap.c`, `mud_comm.c`, `player.c`, `adminlist.c`, `track.c`,
`build.c`, `stat_obj.c`, `renumber.c`, `services.c`, `mpxset.c`,
`shops.c`, `ibuild.c`, `ident.c`, `interp.c`

## Remaining work

- IMC sources (`imc*.c`) — MUD-network federation, deferred to post-core
- Broader post-combat gameplay: additional encounters, spells, mobprogs,
  and longer session stability

## Reproducing the runtime

```sh
mkdir -p /tmp/smaug_run && cd /tmp/smaug_run
for d in gods player boards classes clans races; do
  ln -sfn /path/to/MadSMAUG/upstream/smaug1.8/$d $d
done
cp -rL /path/to/MadSMAUG/upstream/smaug1.8/area area
cp -rL /path/to/MadSMAUG/upstream/smaug1.8/system system
madc /path/to/MadSMAUG/src/SMAUG.mad
```

## How the estimate is maintained

- **Parse/Compile %**: bumped when a new upstream file is added to
  `SMAUG.mad` and compiles cleanly. LoC counts from
  `wc -l upstream/smaug1.8/src/*.{c,h}`.
- **Link %**: bumped as undefined-symbol errors are resolved.
- **Runtime %**: bumped as runtime milestones are reached (login,
  movement, combat, persistence).

Precision doesn't matter much — the goal is direction and a shared
picture of how close we are.
