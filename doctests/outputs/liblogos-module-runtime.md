# Running a Real Module Against This liblogos

`logos-liblogos` is the core of the Logos platform — `logos_host` and the
`liblogos_core` C API that every frontend (the `logoscore` CLI, the basecamp
desktop app) builds on. This doc-test exercises **this** liblogos commit
end-to-end through the headless `logoscore` runtime:

1. Build the `logoscore` CLI, **overriding its `logos-liblogos` input with the
   commit under test** — so the runtime you exercise is built against the code
   in this repository, not the latest published release.
2. Build the `lgpm` local package manager.
3. Build the real [`accounts_module`](https://github.com/logos-co/logos-accounts-module)
   as an `.lgx` package straight from its own flake, and install it into a
   `./modules` directory with `lgpm`.
4. Start `logoscore` in daemon mode (`-D`), load `accounts_module`, introspect
   it, and call one of its methods — verifying the module actually runs on top
   of this liblogos.

Because every layer (host, module loader, IPC) comes from the liblogos commit
under test, a green run is real evidence that this change keeps the module
runtime working.

**What you'll build:** The real `accounts_module`, installed with `lgpm` and called through a `logoscore` daemon running on this liblogos commit.

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

- **git** — to clone the module repository.
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

### 1.1 Build the CLI with the liblogos override

```bash
nix build 'github:logos-co/logos-logoscore-cli/b92ade06cdbd3cdf48c8de5b8375cbcc3a6088cf' \
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

## Step 3: Build and install the accounts module

Clone [`logos-accounts-module`](https://github.com/logos-co/logos-accounts-module),
build its `.lgx` straight from its flake's `#lgx` output, and install it
into a local `./modules` directory with `lgpm`. Every module built with
[`logos-module-builder`](https://github.com/logos-co/logos-module-builder)
exposes a ready-to-install `#lgx`.

### 3.1 Clone the module

We clone over HTTPS so the step works in CI; over SSH the URL is
`git@github.com:logos-co/logos-accounts-module.git`.

```bash
git clone --depth 1 https://github.com/logos-co/logos-accounts-module.git
```

### 3.2 Build the module's .lgx

Build the `#lgx` output and link it as `./accounts-lgx`. (This compiles
the module and its SDK dependencies through Nix, so the first build is
slow.)

```bash
# From inside the clone this is simply: nix build '.#lgx'
nix build 'path:./logos-accounts-module#lgx' -o accounts-lgx
```

The `.lgx` package is now under `./accounts-lgx/`:

```bash
ls accounts-lgx/*.lgx
```

### 3.3 Seed the modules directory with the bundled capability module

`accounts_module` is loaded through the host's capability layer, so the
modules directory also needs the `capability_module` that ships with
`logoscore`. Copy it across first.

```bash
mkdir -p modules
cp -RL ./logos/modules/. ./modules/

```

### 3.4 Install the .lgx with lgpm

Install the freshly-built package into `./modules`. `accounts_module` is
a `core` module, so it goes to `--modules-dir`. The package is unsigned
(a local dev build), so we pass `--allow-unsigned`.

```bash
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file accounts-lgx/*.lgx
```

### 3.5 Confirm the install

Scan the directory and confirm the module landed:

```bash
./lgpm/bin/lgpm --modules-dir ./modules list
```

---

## Step 4: Run the daemon and call the module

Start `logoscore` in daemon mode pointed at `./modules`, then use the client
subcommands to load `accounts_module`, introspect it, and call one of its
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

`accounts_module` should be visible in the scan directory:

```bash
logoscore list-modules
```

### 4.5 Load the module

Load `accounts_module` into the running daemon:

```bash
logoscore load-module accounts_module
```

### 4.6 Confirm the module is loaded

Re-run `status`; the module that was `not_loaded` before now reports
`loaded`:

```bash
logoscore status
```

### 4.7 Introspect the module with module-info

`module-info` lists the `Q_INVOKABLE` methods the module exposes — the
same methods you can `call`:

```bash
logoscore module-info accounts_module
```

### 4.8 Call a method

Generate a fresh 12-word BIP-39 mnemonic. `createRandomMnemonic` takes
the word count and returns the phrase — a real round-trip through the
go-wallet-sdk C library wrapped by the module, dispatched over liblogos'
IPC:

```bash
logoscore call accounts_module createRandomMnemonic 12
```

### 4.9 Call a second method

`lengthToEntropyStrength` maps a mnemonic word count to its entropy
strength in bits — 12 words is 128 bits. This exercises an `int`
round-trip:

```bash
logoscore call accounts_module lengthToEntropyStrength 12
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
