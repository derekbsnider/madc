# Security Policy

## Supported versions

madc is developed in the open and released often. Security fixes land in
the next release; only the latest release is supported.

| Version          | Supported |
|------------------|-----------|
| latest release   | yes       |
| earlier releases | no        |

## Reporting a vulnerability

Please report a vulnerability privately, not in a public issue:

1. Open the repository's **Security** tab.
2. Choose **Report a vulnerability**.
3. Describe the problem: the madc version (`madc --version`), the
   platform, and the smallest input that shows it.

A confirmed vulnerability is fixed in the next release, and the release
notes credit the reporter unless they ask otherwise.

## Scope

madc compiles and runs the program it is given, with the rights of the
user who runs it. Running an untrusted program is therefore not a madc
vulnerability, any more than it is one in gcc or a shell.

In scope:

- a crash or memory corruption in madc itself from a source file it only
  compiles (`madc -c`, `madc -o`, `--emit=c11`), not runs;
- an installed package (`.deb`, `.rpm`, tarball, zip) that installs files
  with unsafe permissions or outside the places it names;
- madc reading or writing files outside what the command line names;
- code in `third_party/` as madc ships it (MIR, the webview), reported
  here first.

madcide and Chthonia have their own policies in their repositories.
