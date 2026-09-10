# Proxenos

A self-hosted service manager for Linux. Start, stop, and monitor local services with a GTK window or XFCE panel plugin. Optionally expose them via local DNS and HTTP routing (e.g., `ollama.proxenos` → `127.0.0.1:11434`).

## Quick start

**Requirements:** Linux, GLib 2, GTK 3 (for UI), C17 compiler.

```sh
make
make install
```

By default, installation goes to `~/.local`. Use `PREFIX=/usr DESTDIR="$pkgdir" install` for packaging.

After install, edit `~/.config/proxenos/services.conf` or run `proxenos` to launch the UI.

## Configure a service

Services are defined in an INI-style config file:

```ini
[ollama]
backend=direct
command=ollama serve
port=11434
hostname=ollama.proxenos
```

Common fields:
- `command` — shell command to run (parsed as arguments; use a script if you need shell features)
- `port` — port the service listens on (checked to verify readiness, optional)
- `hostname` — local DNS name for routing (requires router, optional)
- `environment` — semicolon-separated env vars
- `working_directory` — working directory for the command
- `stop_signal` — TERM (default) or KILL
- `stop_timeout_seconds` — grace period before KILL

## Control services

Via CLI:

```sh
proxenos-cli up ollama
proxenos-cli up ollama --wait     # wait for readiness
proxenos-cli status ollama
proxenos-cli ready ollama         # run readiness check now
proxenos-cli down ollama
proxenos-cli list
```

Or use the `proxenos` GUI — displays status, logs, and on/off toggles. Closing the window leaves it in the system tray.

## Local names and routing

Enable the **Enable local names** button in the GUI to start the router (requires sudo). This:
- Answers DNS for `*.proxenos` with `127.0.0.1`
- Forwards HTTP requests by `Host` header to the matching local port

So `http://ollama.proxenos` routes to port 11434, and `http://forgejo.proxenos` to port 3000, without binding those services to `0.0.0.0`.

**Note:** Requires root access (Polkit) and only supports HTTP in this release.

## Readiness checks

Proxenos detects readiness in two ways:

1. **Port check (default)** — Reads `/proc/net/tcp[6]` to verify the service's port is actually listening. No connection is made; this costs about 1ms and works for any service.

2. **Custom check** — Set `ready=command` to run a command (exit 0 = ready). Runs once at startup, then every 30s if failing. Results are cached so the UI doesn't re-run it constantly.

For a service without a declared port, Proxenos just checks if the process exists.

## Build and test

```sh
make          # build all binaries
make check    # run tests (tests/ directory)
make install  # install to $PREFIX (default: ~/.local)
make uninstall
```

Tests are unit tests — they don't bind ports or read real config.

## Environment variables

- `PROXENOS_CONFIG` — config file path (default: `~/.config/proxenos/services.conf`)
- `PROXENOS_BIN` — path to `proxenos-cli` if not on `PATH`
- `PROXENOS_DEBUG_LOG` — debug log location (default: `~/.local/state/proxenos/debug.log`)

## License

MIT. See `LICENSE`.
