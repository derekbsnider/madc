# The command line

madc reads its command line the way gcc does, on Linux, macOS and Windows
alike. Its options are one table (`src/madc_cli.cpp`) read by one reader
(`include/libmadc/cli_options.h`); `madc --help`, `madc --help=json` and the
shell completions are written from the same table.

## Syntax

- `-o FILE`: the value is the next word, whatever it holds (`-o -x` names the
  file `-x`, as gcc does).
- `-IDIR` or `-I DIR`; `-DNAME[=VALUE]` or `-D NAME`; `-LDIR` or `-L DIR`.
- `-lNAME`, `-Wl,...`, `-fno-builtin-NAME`, `-O[LEVEL]`, `-W<name>`: the value
  is glued on.
- `--std=VALUE` (gcc's `-std=VALUE` too), `-stdlib=VALUE`, `--emit=VALUE`, and
  the other `NAME=VALUE` options: the value follows the `=`.
- `--run-frozen[=FILE]`, `--dump-forest[=FILE]`, `--forest-bind[=FILE]`,
  `-fdiagnostics-color[=WHEN]`, `--help[=json]`: the value may be left out.
- When more than one spelling could read a word, a whole-word spelling wins,
  then `NAME=VALUE`, then the longest glued spelling: `-Werror=x` is
  `-Werror`, not `-W`.
- No abbreviations: `--verb` is not `--verbose`.
- `--` ends madc's options.
- On a line that builds, with `-c`, `-o`, `-shared`, `-r`, `-E`, `-dM`,
  `--emit-object`, `--emit-executable` or `--dump-source`, options may follow
  the sources, as gcc's do. The same holds on a compiler line: a C or C++
  source with one of those options after it.
- On a line that runs a program, the program's file ends madc's options. Every
  later word is the program's argv, untouched: `madc tool.mad -o out` runs
  `tool.mad` with `-o out`.

## Response files

`@FILE` is replaced by FILE's words, as gcc does it:
- blanks separate words, and `'...'` and `"..."` quote;
- a backslash makes the next character literal everywhere, inside quotes too.
  Write paths with `/`, or double each `\`.

A file may name another file. One that names itself stops with
`too many @-files encountered`. A name that cannot be opened, or that is not
a regular file (a device, a FIFO), stays as written; a directory is an error.
Response files expand only among madc's options, never inside a program's argv.

## Errors

madc exits 1 and names the problem as gcc does:

- `madc: error: unrecognized command-line option '--verb'`, with
  `; did you mean '--verbose'?` when a spelling is close;
- `madc: error: missing argument to '-o'`;
- `madc: error: invalid value '4' for '-O': expected one of 0, 1, 2, 3`.

## Help, JSON and completions

- `madc --help`: wrapped to `$COLUMNS`, or 80 when it is unset.
- `madc --help=json`: every option as data (name, spellings and their forms,
  metavar, help, type, choices), grouped by section. Tools and madcide read
  this instead of copying the table.
- `madc --completion=SHELL`, for bash, zsh, fish or powershell:
  - bash: `madc --completion=bash > ~/.local/share/bash-completion/completions/madc`
  - zsh: `madc --completion=zsh > ~/.zfunc/_madc`, with `~/.zfunc` on `fpath`
  - fish: `madc --completion=fish > ~/.config/fish/completions/madc.fish`
  - PowerShell: add `madc --completion=powershell | Out-String | Invoke-Expression`
    to `$PROFILE`
