# URI Channels — `madc::channel`

`madc::channel` is one URI-addressed byte channel with line helpers —
the script door to the host's data-channel registry. The same class is
the embedding host's convenience wrapper (`include/madcdis/channel.h`).
It lives in the `<ns_madc>` embedded header — auto-included on first
`madc::` use in the default dialect; standards modes need the explicit
`#include <ns_madc>`.

One constructor argument is a URI; the scheme picks the transport:

| Scheme | Opens | Notes |
|---|---|---|
| `file://` | a filesystem path | also `pipe://` for FIFOs |
| `tcp://host:port` | a TCP connection | connect-only (no listener surface yet) |
| `udp://host:port` | a UDP socket | datagram semantics |
| `uds://path` / `unix://path` | a Unix-domain socket | |
| `exec://cmd args` | a child process | write → stdin, read → stdout |

## httpget in a few lines

```cpp
madc::channel http("tcp://example.com:80");
http.write("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");
http.close_write();

string line;
while ( http.readline(line) )
    printf("%s\n", line.c_str());
```

`readline` strips the trailing newline (and a preceding `'\r'`, so HTTP
CRLF lines come back clean), returns the final unterminated tail, and
returns `false` only at EOF. The hermetic suite twin is
`tests/testhttpget.mad` (a loopback listener serving one canned
response); this real-host form is documentation, not a test.

## Filtering through a child process

```cpp
madc::channel sorter("exec://sort");
sorter.write("pear\napple\n");
sorter.close_write();               // EOF to the child

string line;
while ( sorter.readline(line) )
    printf("%s\n", line.c_str());   // apple, pear
```

`exec://` semantics (deliberate):

- The URI path splits on **single spaces** into argv (`exec://sort -r`).
  This is **not a shell** — no quoting, no globbing, no variables, no
  redirection. An argument that itself contains a space cannot be
  expressed in the URI form (embedders use `ProcessOptions.args`).
- The child's **stderr is inherited**, not piped — diagnostics stay
  visible and an undrained stderr can never block a chatty child.
- A command with no `/` resolves against the spawn environment's `PATH`
  (`posix_spawnp` shape). A spawn that cannot succeed makes the
  constructor fail loudly: `ok()` is false and `last_error()` says why.
- Closing the channel sends stdin EOF and reaps the child.

## Surface

| Member | Semantics |
|---|---|
| `channel(uri)` / `channel(uri, mode)` | modes `"r"`, `"w"`, `"rw"` (default), `"a"` |
| `ok()` | open succeeded and no error is latched |
| `last_error()` | the latched error message (`""` when clean) |
| `read(buf, cap)` | bytes read; `0` at EOF, `-1` on error |
| `readline(out)` | one line, newline stripped; `false` at EOF |
| `readall(out)` | drains the rest of the stream |
| `write(text)` / `write(buf, n)` | C string, `string`, or counted bytes |
| `close_write()` | flush + half-close (EOF to the peer / child) |
| `close()` | full close; the destructor calls it |

Channels are **not copyable** — pass by reference. Buffered `readline`
bytes are served to a later `read()` before the wire is touched, so the
two styles mix safely.

## Tests

`tests/testexecchannel.mad`, `tests/testtcpchannel.mad`, and
`tests/testhttpget.mad` are the suite legs — all hermetic (loopback
listeners built with raw libc `socket`/`bind`/`listen` calls in the same
script; no external network, no fixtures beyond `.expect`).
