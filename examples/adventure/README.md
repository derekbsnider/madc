# Colossal Cave Adventure — the madc port

The 430-point **Adventure 2.5** (1995), the last version by Will Crowther
and Don Woods, implemented in madc on the `madc::ui` hub — data-driven,
ECS-shaped, MUD-seamed. Plan and architecture:
[docs/plans/2026-08-20-adventure-430-plan.md](../../docs/plans/2026-08-20-adventure-430-plan.md).

## Layout

| Path | What |
|------|------|
| `data/adventure.yaml` | The dungeon, verbatim from [open-adventure](https://gitlab.com/esr/open-adventure/) (BSD-2 — see `data/COPYING`) |
| `tools/advent_yaml_to_world.py` | Offline converter: adventure.yaml → adventure.world (shape-checked: 185 locations, 70 objects, 623 travel rules, 76 motion groups, 58 actions) |
| `adventure.world` | The generated hub world file, CHECKED IN. A fixed point of madc's own save: `ui::world_open` + `ui::world_save` re-emits it byte-identically |

## Regenerating the world

```
python3 tools/advent_yaml_to_world.py data/adventure.yaml adventure.world
```

Requires PyYAML. The converter refuses on any shape drift from the
vendored YAML.

## Attribution

Colossal Cave Adventure was written by **Will Crowther** (1976) and
expanded by **Don Woods** (1977–1995). The dungeon data and the
regression-transcript oracle come from **Eric S. Raymond's
open-adventure** project, which forward-ported Adventure 2.5 with the
authors' blessing. All of it is 2-clause BSD — `data/COPYING` rides
along. The oracle law for this port: the in-scope open-adventure
regression transcripts (`win430` included) must play through the madc
engine byte-identically.
