# Proxenos

Proxenos is a per-user manager for local self-hosted services on Linux. It
starts and stops configured commands, records their output, reports whether
each one can actually answer (not just whether the process exists), and
offers a GTK window and an XFCE panel menu of on/off toggles. Nothing starts
automatically at login — you turn services on when you want them.

Requires GLib 2, GTK 3, XFCE Panel 4.18, a C17 compiler, and Linux (the
process and readiness checks read `/proc`). Developed on Manjaro with XFCE.

## Build and install

Each program links only what it needs: `proxenos-cli` and `proxenos-router`
need GLib alone, the window needs GTK 3, and the panel module alone needs the
XFCE panel headers. Build products go under `build/`, which is gitignored.

```sh
make
make check
make install
```

`make check` runs the tests in `tests/` — the shared helpers against a
temporary state directory, and the router's DNS/HTTP parsing against
constructed packets and requests. Neither binds a real port or reads your
actual config.

`make install` installs to `~/.local` by default. `PREFIX` and `DESTDIR` both
apply, so a distribution package can stage the same tree elsewhere:

```sh
make PREFIX=/usr DESTDIR="$pkgdir" install
```

A home-directory install also seeds `~/.config/proxenos/services.conf` from
`config/services.conf.example` the first time, and never overwrites it after
that. A packaging build (with `DESTDIR` set) writes nothing into any home
directory; the example ends up at `$PREFIX/share/proxenos/services.conf.example`
either way, for the user to copy later.

The panel module ships with the `.desktop` file XFCE needs to list it, so
**Proxenos** shows up under Panel Preferences → Add New Items once the panel
is restarted (`xfce4-panel -r`).

`make uninstall` removes the installed programs and desktop files, honouring
the same `PREFIX`/`DESTDIR`. It leaves `services.conf` and your registrations
alone.

`packaging/` holds a `PKGBUILD` and a Polkit policy file for a distribution
package; they live in the repo rather than being published anywhere yet.

## Register a service

Services live in an INI-style GLib key file, chosen over YAML because the C
runtime already parses it safely and values can carry escaped newlines
(`\n`) when a command needs more than one line.

```ini
[ollama]
backend=direct
command=nix develop /path/to/ollama-flake --command ollama serve
working_directory=/path/to/ollama-flake
environment=OLLAMA_VULKAN=1;OLLAMA_INTEL_GPU=1;
port=11434
hostname=ollama.proxenos
stop_signal=TERM
stop_timeout_seconds=10
ready=port
ready_timeout_seconds=30
```

`command` is parsed as argv, not handed to a shell — use a small script if
you need pipes, globs, or other shell features. `environment` and `logs` are
semicolon-separated lists. `backend` exists so that a future backend (a
Pitchfork adapter, say, or a Proxenos-owned daemon) can replace the direct
launcher without touching anyone's service file; `direct` — run the command
yourself — is the only one implemented, and the default if you omit the
field. `ready`, `ready_command`, and `ready_timeout_seconds` control the
readiness check; see below. `config/services.conf.example` (installed
alongside the binary) walks through a second example, Forgejo, where a
command check earns its keep.

The order of `[group]` blocks in the file is the order the window and the
panel menu list services in. Dragging a row in the window moves the group in
the file, so the file and the two menus never disagree, and the order
survives a restart. Rows are dragged by their body — the switch and the
`Logs` button keep their own clicks, so there's no way to start a drag by
reaching for a control.

That move edits the file's text directly rather than reparsing it and
writing it back, so comments, spacing, and the exact wording of every line
survive. A block takes with it the comment lines directly above its header
(with no blank line in between — where a note about a service normally
goes); anything above the first group stays at the top. A comment at the end
of a block, such as a commented-out example, belongs to that block and
travels with it — worth remembering if you want such a comment to stay put
at the bottom of the file.

Run it with:

```sh
proxenos-cli up ollama
proxenos-cli up ollama --wait
proxenos-cli status ollama
proxenos-cli ready ollama
proxenos-cli down ollama
proxenos-cli list
```

## Readiness

Whether a service is running is a question about a process. Whether it can
answer is a question about its port, and Proxenos settles that one without
sending the service anything: the kernel already lists every listening
socket in `/proc/net/tcp` and `/proc/net/tcp6`, so Proxenos reads that list
and looks for the declared port — no connection made, nothing in the
service's access log. The socket's inode is then matched against the
service's own file descriptors, which is what tells "your service is
listening" apart from "something is listening on that port" (a collision,
which Proxenos reports as such rather than as ready). Building those tables
costs about a millisecond regardless of how many services you have, since
the kernel walks every socket on the machine either way — so Proxenos reads
them once per refresh and checks each service against that one snapshot.

That check can't see everything: a web app whose database connection is
down still listens on its port and still fails every real request. For
those, set `ready=command` and `ready_command=...`; exit status 0 means
ready. It runs once when `proxenos-cli up --wait` is waiting on a service,
once when the window notices a service's port has newly opened, and every
30 seconds thereafter for as long as it keeps failing — never on a steady
timer once it has passed. The result is cached next to the pid file, so the
window and panel show what the last check found instead of re-running it on
every redraw.

```sh
proxenos-cli up forgejo --wait   # returns once ready, or exit 4 on timeout
proxenos-cli ready forgejo       # run the check now and record the answer
```

`proxenos-cli list` and `status` report one of `stopped`, `starting`,
`running` (alive, with no port to check), `ready`, or `port taken`. Exit
status is 0 for ready/running, 3 for stopped or not-yet-ready, and 4 when a
service started but never became ready — including when it exited during
startup, in which case the exit status it left is reported alongside.

## The window and panel

Launch the window with `proxenos`. Each row shows the service's name and
current state on one line, with its declared endpoint underneath; the
controls are a `Logs` button and an on/off switch. The switch reflects
whether the process exists; the state text next to the name reflects
whether it can answer.

`Logs` opens a small viewer rather than a notice. It lists the captured
output location and any `logs=` paths, each of which copies itself to the
clipboard on click, followed by the last 300 lines of captured output
(`Refresh` re-reads without closing the window). Reading starts at most 256
KiB before the end of the file, so a log that's been running for weeks
opens as fast as one from this morning — if that cutoff lands mid-line, the
partial line is dropped rather than shown as if it were whole. The `logs=`
paths are listed to be copied, not read: their format belongs to the
service, not to Proxenos.

Set `PROXENOS_BIN` if `proxenos-cli` isn't on the window's `PATH` — the
window runs no checks itself, it asks `proxenos-cli` for everything, so an
older `proxenos-cli` than the window expects will silently refuse the
`ready` verb and configured checks won't run.

The window's header shows the build's Git revision, with a `-dirty` suffix
if the working tree had changes when it was built. **Local names** reports
active only once both the router and the persistent DNS integration are in
place; a router running with incomplete DNS setup is reported separately,
with a repair action offered.

Closing the window doesn't quit it — it stays in XFCE's notification area
(or, with none available, closing performs the same shutdown described
next). Its tray menu offers **Stop services and quit**, which stops every
running service and the local-names router before exiting.

A termination signal (`TERM`, `INT`, `HUP`) triggers that same shutdown, so
logging out of XFCE stops the services Proxenos started rather than
orphaning them. Each `down` gets its own session before it runs, so a
graceful stop that takes the service's full timeout still finishes even
after the window itself is gone. Stopping the router is attempted too, but
that needs a Polkit prompt, and at logout there may be no agent left to
answer it — the services stop regardless. A service only stops when
Proxenos decides to stop it, so killing the window with `KILL`, or a crash,
leaves services running.

Proxenos captures each service's stdout/stderr to
`$XDG_STATE_HOME/proxenos/<service>.log` (normally
`~/.local/state/proxenos/<service>.log`) and tracks the child process next
to it. `down` sends the configured stop signal, waits out the timeout, then
sends `KILL`. Only set `logs=` for a service that writes its own log files
natively — Proxenos's own capture needs no such declaration.

The tracking file records the process's start time next to its pid, because
a pid alone isn't an identity — the kernel reuses numbers once a process
exits, and without the start time a `down` after an unclean exit could
signal whatever unrelated process had since inherited that pid. A file left
by an earlier Proxenos version, with a pid and nothing else, is still read
and trusted.

`up` reports a command it couldn't start: the child sends the reason back to
`proxenos-cli` before its output gets redirected into the service log, so a
misspelled command or an unreachable working directory is named on the
terminal rather than leaving a service that looks like it started.

### Icons

`make install` also installs a **Proxenos** desktop launcher and two SVGs of
the same mark — a gold hand opening a dark threshold within a gold civic
seal. The launcher and the window use `proxenos.svg` (`#D6BA7C`, the full
gold version), since a taskbar or application menu shows each program in its
own colors. The tray icon and the one at the top of the window use
`proxenos-symbolic.svg`, which carries no color of its own: GTK draws any
icon whose name ends in `-symbolic` in the foreground color of whatever
widget holds it, so those two follow the active GTK theme. The launcher
appears in the XFCE application menu once the desktop entry cache refreshes
(or after logging out and back in).

`make install` rebuilds `$(DATADIR)/icons/hicolor/icon-theme.cache` as its
last step, because GTK reads that cache instead of the directory listing
whenever the cache is at least as new as the theme root — and installing an
icon only touches the `scalable/apps` subdirectory beneath it. An icon
installed next to a stale cache is therefore invisible to lookups, and
Proxenos falls back to the gold icon when the symbolic one can't be found,
so the symptom is a tray icon that never changes color rather than an
error. A packaging build skips this step and leaves the cache to the
package manager's own hook.

## Local names and routing

The **Enable local names** button in the window starts Proxenos Router,
which provides two adapters for HTTP services: it answers DNS queries for
`*.proxenos` with `127.0.0.1`, then forwards an HTTP request by its `Host`
header to the matching configured local port. Ollama stays reachable at
`http://127.0.0.1:11434`; once the router is running, `http://ollama.proxenos`
also works, and Forgejo can use `http://forgejo.proxenos` while staying
bound to port 3000 — neither service needs to bind `0.0.0.0`.

The router binds loopback ports 53 (DNS) and 80 (HTTP), so it needs elevated
permission — clicking the button opens a Polkit prompt, points
NetworkManager at the already-running `systemd-resolved`, adds the
`~proxenos` routing domain for `127.0.0.1`, and starts the router. Proxenos
never receives or stores the password itself. It saves the prior
`/etc/resolv.conf` to `/etc/resolv.conf.proxenos-backup` before linking that
file to the standard `systemd-resolved` stub.

The password dialog belongs to XFCE's Polkit agent, not to Proxenos. If
clicking the button produces no prompt, check that an agent is running with
`pgrep -af 'polkit.*agent|xfce-polkit'`, and install one (`polkit-gnome` is
a common Arch/Manjaro choice) if that comes back empty. `proxenos-manjaro-routing`
runs the same setup from a terminal, without the window.

The router accepts HTTP only in this release — no HTTPS, no automatic start
at login, no home-network DNS yet. A configured hostname with no route
returns 403; a routed but unavailable service returns 503.

It serves DNS and HTTP on one non-blocking loop, so a browser holding a
connection open doesn't delay anything else. Up to 64 connections are held
at once — further ones wait in the listen queue rather than being accepted
and starved — and a connection idle for two minutes is closed. Routing is
decided from the first request on a connection, so a browser reusing one
connection for one hostname works normally, but a client that reused a
single connection across two different `.proxenos` names would only reach
the first.

Within the `.proxenos` zone, every name resolves to `127.0.0.1` whether or
not a service is registered for it, so an unregistered name meets the
router's own 403 rather than a name that fails to resolve. A query for a
record type the zone doesn't hold — the AAAA half of an ordinary lookup, say
— is answered "no such record" rather than "no such name," since the latter
would contradict the A answer sent alongside it. Names outside the zone are
refused outright: the router isn't a general resolver.

`--dns-port`, `--http-port`, and `--pid-file` let a second router run
alongside the installed one on unprivileged ports, which is how the tests
and any development instance avoid needing root.

Router and window diagnostics are appended to
`$XDG_STATE_HOME/proxenos/debug.log`
(`~/.local/state/proxenos/debug.log` by default). Set
`PROXENOS_DEBUG_LOG=/path/to/proxenos-debug.log` before launching Proxenos
for development or a shared mount — the window passes its selected log path
to the privileged router, so both use the same file. That path is opened by
a process running as root, so keep it somewhere only you can write; a path
under `$XDG_STATE_HOME` is the safe default and the reason it's the
default. If **Local names** reports a port already in use, inspect the
listeners with `sudo ss -ltnup | grep -E ':(53|80)\b'` and compare against
the debug log.

## Environment variables

- `PROXENOS_CONFIG` — path to the service config file (default: `~/.config/proxenos/services.conf`)
- `PROXENOS_BIN` — path to `proxenos-cli`, when it isn't on the window's or panel's `PATH`
- `PROXENOS_DEBUG_LOG` — path to the diagnostic log (default: `~/.local/state/proxenos/debug.log`)

All three are read at startup by every part of Proxenos — the CLI, the
window, the panel plugin, and the router — so a value set for one and not
another is the usual explanation when two views of Proxenos disagree about
what's registered.

## What's implemented

`backend=direct` — running the command directly — is the only backend so
far; the field exists so a future backend can be added without changing
service files. Readiness checks, port collision detection, and local DNS/HTTP
routing are implemented as described above. HTTPS, home-network DNS, and a
friendlier response page for routing errors are still ahead.

## License

MIT. See `LICENSE`.
