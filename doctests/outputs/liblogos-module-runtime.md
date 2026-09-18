# Running a Real Module Against This liblogos

`logos-liblogos` is the core of the Logos platform — `logos_host` and the
`liblogos_core` C API that every frontend (the `logoscore` CLI, the basecamp
desktop app) builds on. This doc-test exercises **this** liblogos commit
end-to-end through the headless `logoscore` runtime:

1. Build the `logoscore` CLI, **overriding its `logos-liblogos` input with the
   commit under test** — so the runtime you exercise is built against the code
   in this repository, not the latest published release.
2. Build the `lgpm` local package manager.
3. Build [`test_basic_module`](https://github.com/logos-co/logos-test-modules)
   as an `.lgx` package straight from its own flake, and install it into a
   `./modules` directory with `lgpm`.
4. Start `logoscore` in daemon mode (`-D`), load `test_basic_module`, introspect
   it, and call two of its methods — verifying the module actually runs on top
   of this liblogos.

Because every layer (host, module loader, IPC) comes from the liblogos commit
under test, a green run is real evidence that this change keeps the module
runtime working.

**What you'll build:** A real module — `test_basic_module` — installed with `lgpm` and called through a `logoscore` daemon running on this liblogos commit.

**What you'll learn:**

- How to build `logoscore` against a specific `logos-liblogos` commit via `--override-input`
- How to build a real module's `.lgx` from its own flake
- How to install an `.lgx` into a modules directory with `lgpm`
- How to start the `logoscore` daemon, load a module, and call its methods

## Prerequisites

- **Nix** with flakes enabled. Install from [nixos.org](https://nixos.org/download.html), then enable flakes:

```bash
mkdir -p ~/.config/nix
echo 'experimental-features = nix-command flakes' >> ~/.config/nix/nix.conf
```

Verify: `nix flake --help >/dev/null 2>&1 && echo "Flakes enabled"`

- A Linux or macOS machine.

---

## Step 1: Build logoscore against this liblogos

Build the `logoscore` CLI from its published flake, but **override its
`logos-liblogos` input** so it links against the commit under test rather
than the latest release. The result is symlinked to `./logos/`.

> The override URL is what pins liblogos to a specific commit: the doc-test
> runner expands a release placeholder on it to a concrete ref. Locally that
> is this checkout's `HEAD` (see `run.sh`); in CI the workflow first selects
> the repository under test, then pins its commit. With no pin it falls back
> to that repository's latest default branch.

> The CLI itself tracks `master`, deliberately. Only `logos-liblogos` is
> overridden, so everything else in the closure must be a revision that
> liblogos' current runtime pairs with — the shared-runtime split landed as
> one pin set across logos-protocol, logos-plugin-qt, logos-liblogos and
> this CLI, and an older CLI rev pinned here silently reintroduces a second
> copy of the runtime in the process.

### 1.1 Build the CLI with the liblogos override

```bash
nix build 'github:logos-co/logos-logoscore-cli' \
  --override-input logos-liblogos 'github:logos-co/logos-liblogos' \
  --out-link ./logos
```

The build produces `logos/bin/logoscore` plus bundled runtime libraries
and a `logos/modules/` directory containing the built-in
`capability_module` (required for the auth handshake when loading
modules). Because `follows` propagates the override, the whole
dependency closure — `logos_host`, `liblogos_core`, every module — is
rebuilt against this liblogos.

---

## Step 2: Build the lgpm package manager

`lgpm` installs `.lgx` packages into a modules directory and scans what is
installed. Build it from `logos-liblogos`' own `logos-package-manager`
input and link it as `./lgpm`.

### 2.1 Build lgpm

```bash
nix build 'github:logos-co/logos-package-manager#cli' -o lgpm
```

The executable is at `./lgpm/bin/lgpm`.

---

## Step 3: Build and install a module

Build [`test_basic_module`](https://github.com/logos-co/logos-test-modules)'s
`.lgx` straight from its flake and install it into a local `./modules`
directory with `lgpm`. Every module built with
[`logos-module-builder`](https://github.com/logos-co/logos-module-builder)
exposes a ready-to-install `lgx` output.

### 3.1 Build the module's .lgx

`logos-test-modules` publishes each of its modules under
`modules.<system>.<name>`, so the attribute path carries the Nix system
double — `builtins.currentSystem` supplies it. Link the result as
`./module-lgx`.

```bash
# From inside a single-module repo this is simply: nix build '.#lgx'
SYSTEM=$(nix eval --impure --raw --expr 'builtins.currentSystem')
nix build "github:logos-co/logos-test-modules#modules.$SYSTEM.test_basic_module.lgx" -o module-lgx
```

The `.lgx` package is now under `./module-lgx/`:

```bash
ls module-lgx/*.lgx
```

### 3.2 Seed the modules directory with the bundled capability module

Modules are loaded through the host's capability layer, so the modules
directory also needs the `capability_module` that ships with
`logoscore`. Copy it across first.

```bash
mkdir -p modules
cp -RL ./logos/modules/. ./modules/

```

### 3.3 Install the .lgx with lgpm

Install the freshly-built package into `./modules`. `test_basic_module`
is a `core` module, so it goes to `--modules-dir`. The package is
unsigned (a local dev build), so we pass `--allow-unsigned`.

```bash
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file module-lgx/*.lgx
```

### 3.4 Confirm the install

Scan the directory and confirm the module landed:

```bash
./lgpm/bin/lgpm --modules-dir ./modules list
```

---

## Step 4: Run the daemon and call the module

Start `logoscore` in daemon mode pointed at `./modules`, then use the client
subcommands to load `test_basic_module`, introspect it, and call two of its
methods. Daemon output is captured in `logs.txt`.

### 4.1 Start the daemon

Start logoscore in daemon mode in the background, capturing output to
`logs.txt`:

```bash
logoscore -D -m ./modules > logs.txt &
```

The `-D` flag starts the daemon. The client subcommands below connect to
this running process via the config written under `~/.logoscore/`.

```bash
sleep 3
```

### 4.2 Inspect the startup log

Review the daemon's startup output:

```bash
cat logs.txt
```

### 4.3 Check daemon status

Verify the daemon is running:

```bash
logoscore status
```

### 4.4 List discovered modules

`test_basic_module` should be visible in the scan directory:

```bash
logoscore list-modules
```

### 4.5 Load the module

Load `test_basic_module` into the running daemon:

```bash
logoscore load-module test_basic_module
```

### 4.6 Confirm the module is loaded

Re-run `status`; the module that was `not_loaded` before now reports
`loaded`:

```bash
logoscore status
```

### 4.7 Introspect the module with module-info

`module-info` lists the methods and events the module exposes — the
same methods you can `call`:

```bash
logoscore module-info test_basic_module
```

### 4.8 Call a method

`echo` returns its argument unchanged — a string round-trip out to the
module subprocess and back over liblogos' IPC:

```bash
logoscore call test_basic_module echo hello
```

### 4.9 Call a second method

`addInts` adds its two arguments. This exercises an `int` round-trip,
where `echo` exercised a string:

```bash
logoscore call test_basic_module addInts 40 2
```

### 4.10 Stop the daemon

Shut the daemon down cleanly:

```bash
logoscore stop
```

The daemon removes its state file and exits.

```bash
sleep 2
```

### 4.11 Confirm the daemon has stopped

With no daemon running, the client reports `not_running` and exits
non-zero, so we add `|| true` to let the doc-test assert on the output:

```bash
logoscore status
```
