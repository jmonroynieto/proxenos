# Proxenos

An application arrives on your machine as a stranger. Proxenos is the citizen
who gives it a place — a port, a name, a way to be switched on — and stands
surety for it without naturalising it. It is not a daemon holder.

Proxenos is a per-user facilitator for local self-hosted services. It starts
and stops configured commands, records their output, reports whether each one
can actually answer, and offers a GTK window and an XFCE panel menu of
toggles. Nothing starts automatically at login.

Requires GLib 2, GTK 3 and XFCE Panel 4.18, a C17 compiler, and Linux: the
process and readiness checks read `/proc`. Developed on Manjaro with XFCE.

## Build and install

Each program links only what it uses: `proxenos-cli` and `proxenos-router`
need GLib alone, the window needs GTK 3, and the XFCE panel headers are needed
only by the panel module. Build products are kept under `build/`, which is
ignored by Git.

```sh
make
make check
make install
```

`make check` runs the tests in `tests/`: the shared helpers against a
temporary state directory, and the router's DNS and HTTP parsing against
constructed packets and requests. Neither test binds a port nor reads the real
configuration.

`make install` installs under `~/.local` by default, because a per-user
facilitator belongs in the account that runs it. `PREFIX` and `DESTDIR` both
apply, so a distribution package stages the same tree:

```sh
make PREFIX=/usr DESTDIR="$pkgdir" install
```

A home-directory install also seeds `~/.config/proxenos/services.conf` from
the example the first time, and never overwrites it afterwards. A packaging
build sets `DESTDIR` and writes nothing into any home directory; the example
is installed to `$PREFIX/share/proxenos/services.conf.example` either way.

The panel module is installed with the `.desktop` file XFCE needs to list it,
so **Proxenos** appears in Panel Preferences → Add New Items after the panel
is restarted (`xfce4-panel -r`).

`make uninstall` removes the installed programs and desktop integration,
honouring the same `PREFIX` and `DESTDIR`. It preserves `services.conf` and
the registrations in it.

`packaging/` holds a `PKGBUILD` and a Polkit action for a distribution
package. They are kept in the repository rather than published anywhere.

## Register a service

The configuration is an INI-style GLib key file. It was chosen over YAML for
the first version because the C runtime already parses it safely and values may
include escaped newlines (`\n`) when needed.

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
```

`command` is parsed as a command and its arguments; it is never implicitly
given to a shell. Use a small script as the command when shell features are
needed. `environment` and `logs` are semicolon-separated lists.

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
sending the service anything: the kernel already lists every listening socket
in `/proc/net/tcp` and `/proc/net/tcp6`, so Proxenos reads that list and looks
for the declared port. No connection is made, nothing appears in the service's
access log, and the service does no work. The socket's inode is then looked up
in the service's own file descriptors, which is what distinguishes *your
service is listening* from *something is listening on that port* — the second
is a collision, and Proxenos says so rather than reporting readiness.

The kernel builds each of those tables by walking every socket on the machine,
which costs about a millisecond whether you have one service or ten. So the
tables are read once per refresh and asked about each service in turn, and the
cost does not grow with the number of registrations.

That check cannot see everything. A web application whose database is down
listens on its port and fails every real request. For those, `ready=command`
names a command whose exit status is the answer, on the shell's usual terms:
zero means ready. It runs during `proxenos-cli up --wait`, once when a service
first starts listening, and again every thirty seconds while it is failing —
never on a steady timer once it has passed. The result is recorded beside the
pid file, so the window and the panel report what the last check found without
running it again.

```sh
proxenos-cli up forgejo --wait   # returns when it is ready, or 4 on timeout
proxenos-cli ready forgejo       # run the check now and record the answer
```

`proxenos-cli list` and `status` report `stopped`, `starting`, `running` (for a
service with no port to check), `ready`, or `port taken`. Exit status is 0 for
ready or running, 3 for stopped or not ready, and 4 when a service started but
never became ready — including when it exited during startup, in which case the
status it exited with is reported. `config/services.conf.example`, installed as
`$PREFIX/share/proxenos/services.conf.example`, documents the `ready`,
`ready_command` and `ready_timeout_seconds` fields and when each is worth
using.

Version 0.1's contract said a successful launch means only that the process was
started. With a port declared, it now also means the port is open.

Launch the windowed controller with `proxenos`. It displays service state,
declared endpoint, the captured-output location, configured native log
locations, and an on/off switch. The switch says whether the process exists;
the line under the name says whether it can answer. Set `PROXENOS_BIN` if
`proxenos-cli` is not on the UI process's `PATH` — the window runs no checks
itself, it asks `proxenos-cli`, so an installed `proxenos-cli` older than the
window will refuse the `ready` verb and configured checks will not run.

The header displays the build's Git revision. A `-dirty` suffix means the
working tree contained changes when it was built. Local names reports **active**
only when both Proxenos Router and the persistent DNS integration are present;
it reports a running router with incomplete DNS setup separately and offers a
repair action.

Closing the window keeps Proxenos in XFCE's notification area. Its tray menu
offers **Stop services and quit**, which stops every running registered service
and the local-name router before exiting. If no notification area is available,
closing the window performs that same shutdown.

A termination signal means the same thing. Proxenos handles TERM, INT and HUP
by running that shutdown, so logging out of XFCE stops the services it started
rather than leaving them running with no window to manage them. Each `down` is
given its own session before it runs, so a graceful stop that takes the
service's full timeout finishes even though the window has already gone.
Stopping the router is attempted too, but that needs an authentication prompt,
and at logout there may no longer be an agent to answer it; the services stop
either way. A service is only stopped by Proxenos deciding to stop it, so a
window killed with KILL, or a crash, still leaves services running.

`make install` also installs a **Proxenos** desktop launcher and its second-
stage SVG icon: a civic seal, hand, and open threshold in `#D6BA7C`. It will
appear in the XFCE application menu after the desktop entry cache refreshes
(or after logging out and back in).

Proxenos writes captured stdout and stderr to
`$XDG_STATE_HOME/proxenos/<service>.log` (normally
`~/.local/state/proxenos/<service>.log`) and tracks the child process alongside
it. `down` uses the configured stop signal, waits for the timeout, then sends
KILL. Only add `logs=` for a service that actually writes its own native log
files.

The tracking file records the process start time next to its pid. A pid on its
own is not an identity, because the kernel hands the number out again once a
process is gone; without the start time, a `down` after an unclean exit could
signal whatever unrelated process had since inherited the number. A file
written by an earlier version, holding a pid alone, is still read and trusted.

`up` reports a command it could not start. The child sends the reason back to
`proxenos-cli` before its output is redirected into the service log, so a
misspelled command or an unreachable working directory is named on the terminal
rather than leaving a service that appears to have started.

## Endpoints and hostname routing

Proxenos Router provides the two adapters for HTTP services: it answers DNS
queries for `*.proxenos` with `127.0.0.1`, then forwards an HTTP request by its
`Host` name to the matching configured local port. Ollama remains available at
`http://127.0.0.1:11434`; HTTP clients may also use
`http://ollama.proxenos` while the router is running. Forgejo can use
`http://forgejo.proxenos` while remaining bound to port 3000.

The router binds to loopback ports 53 (DNS) and 80 (HTTP), so it needs elevated
permission. The **Enable local names** button in the Proxenos window starts
and stops it. Clicking it opens the system Polkit authentication prompt,
configures NetworkManager to use the already-running systemd-resolved service,
adds the `~proxenos` routing domain for `127.0.0.1`, and starts the router.
Proxenos never receives or stores the password. It saves the prior resolver
contents to `/etc/resolv.conf.proxenos-backup` before linking `/etc/resolv.conf`
to the standard systemd-resolved stub.

The password dialog is provided by XFCE's Polkit authentication agent, not by
Proxenos. If clicking the button does not show one, verify that an agent is
running with `pgrep -af 'polkit.*agent|xfce-polkit'`. Install and log back into
an XFCE Polkit agent if that command has no output; `polkit-gnome` is a common
Arch/Manjaro option.

`proxenos-manjaro-routing` remains available for terminal use. The router
accepts HTTP only in this release. A configured hostname with no route returns
403, and a routed but unavailable service returns 503. HTTPS, automatic launch
at login, and home-network DNS remain future work.

The router serves its DNS and HTTP work on one non-blocking loop, so a browser
holding a connection open does not delay anything else. It keeps up to 64
connections at once; further connections wait in the listen queue rather than
being accepted and starved, and a connection idle for two minutes is closed.
Routing is decided from the first request on a connection: a browser reusing
one connection for one hostname is served normally, but a client that reused a
single connection for two different `.proxenos` names would reach only the
first.

Within the `.proxenos` zone every name resolves to 127.0.0.1, whether or not a
service is registered for it, so an unregistered name meets the router's own
403 rather than a name that fails to resolve. A query for a record type the
zone does not hold, such as the AAAA half of an ordinary lookup, is answered as
"no such record" rather than "no such name", which would contradict the A
answer sent alongside it. Names outside the zone are refused: the router is not
a general resolver.

`--dns-port`, `--http-port` and `--pid-file` let a second router run alongside
the installed one on unprivileged ports, which is how the tests and any
development instance avoid needing root.

Router and UI diagnostics are appended to `$XDG_STATE_HOME/proxenos/debug.log`
(`~/.local/state/proxenos/debug.log` by default). For development or a shared
mount, set `PROXENOS_DEBUG_LOG=/path/to/proxenos-debug.log` before launching
Proxenos. The UI passes its selected log path to the privileged router, so both
components use the same file. That path is opened by a process running as root,
so keep it somewhere only you can write; a path under `$XDG_STATE_HOME` is the
safe default and the reason it is the default. If Local names reports that a
port is already in use, inspect the listeners with
`sudo ss -ltnup | grep -E ':(53|80)\b'` and compare the result with the debug
log.

## Environment

`PROXENOS_CONFIG` names the service configuration file, and is honoured by every
part of Proxenos: the command line tool, the window, the panel plugin and the
router. `PROXENOS_BIN` names the `proxenos-cli` the window and panel should run
when it is not on `PATH`. `PROXENOS_DEBUG_LOG` names the diagnostic log. All
three are read at startup, so a value set for one program and not another is the
usual explanation for two views of Proxenos disagreeing about what is
registered.

## License

MIT. See `LICENSE`.

## Current backend boundary

`backend=direct` is implemented. Its configuration field is intentional: a
Pitchfork adapter or a Proxenos-owned daemon backend can replace the direct
launcher without changing registered-service files. Readiness checks, port
collision detection, local DNS, reverse proxying, HTTPS, home-network support,
and the Hegeso response page are the next implementation stages.
