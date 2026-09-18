# Bundles modules from external flake inputs into the logos_host_qt modules directory.
# logos_host_qt expects: modules/<name>/manifest.json + <name>_plugin.{so,dylib,dll}
# When portableBuild is false (default/dev), manifest keys get a "-dev" suffix
# to match the dev variant lookup in platformVariantsToTry().
#
# `modules` is a list of { name; pkg; version; }. Adding an entry here changes
# what `logoscore list-modules` reports, so any doctest pinning that list has to
# move in the same change.
{ pkgs, common, modules, portableBuild ? false }:

let
  # The manifest keys must describe the TARGET, not the machine doing the
  # build. `uname` used to supply them, which is correct natively and wrong
  # under cross-compilation: a Windows build on the Linux builder emitted
  # "linux-x86_64-dev" keys, so PackageManagerLib::platformVariantsToTry()
  # (which returns "windows-x86_64-dev" on Windows) would find no entry and the
  # capability module would silently not load.
  hostPlatform = pkgs.stdenv.hostPlatform;
  platform =
    if hostPlatform.isDarwin then "darwin"
    else if hostPlatform.isWindows then "windows"
    else "linux";
  arch =
    if hostPlatform.isAarch64 then "aarch64" else "x86_64";
  suffix = if portableBuild then "" else "-dev";

  bundleOne = m: ''
    mkdir -p $out/modules/${m.name}

    # Copy the plugin library. Every extension is listed explicitly -- an
    # if/elif chain, or a Unix-only pair of globs, installs NOTHING on Windows
    # and still succeeds.
    shopt -s nullglob
    plugins=(${m.pkg}/lib/*.dylib ${m.pkg}/lib/*.so ${m.pkg}/lib/*.dll)
    for lib in "''${plugins[@]}"; do
      cp "$lib" $out/modules/${m.name}/
    done

    # Determine the plugin filename that was copied
    pluginFile=""
    for f in $out/modules/${m.name}/*; do
      if [ -f "$f" ]; then
        pluginFile="$(basename "$f")"
        break
      fi
    done

    if [ -z "$pluginFile" ]; then
      echo "Error: No ${m.name} library found under ${m.pkg}/lib" >&2
      ls -la ${m.pkg}/lib >&2 || true
      exit 1
    fi

    # "type" is REQUIRED, not decorative. PackageManagerLib::getInstalledModules
    # enumerates manifests with `types = {"core"}` and skips -- silently, with
    # no diagnostic -- every manifest whose type does not match. Without this
    # field the directory scans clean, the module never enters the registry,
    # and the only symptom is a later "Module not found in known modules:
    # <name>" from whoever tried to load it. It matches the "core" in each
    # module's metadata.json, which is what the lgx-bundled manifest carries.
    cat > $out/modules/${m.name}/manifest.json <<EOF
    {
      "name": "${m.name}",
      "version": "${m.version}",
      "type": "core",
      "main": {
        "${platform}-${arch}${suffix}": "$pluginFile",
        "${platform}-amd64${suffix}": "$pluginFile",
        "${platform}-arm64${suffix}": "$pluginFile",
        "${platform}-x86_64${suffix}": "$pluginFile",
        "${platform}-aarch64${suffix}": "$pluginFile"
      }
    }
    EOF
  '';
in
pkgs.runCommand "${common.pname}-modules-${common.version}"
  {
    inherit (common) meta;
  }
  ''
    ${pkgs.lib.concatMapStrings bundleOne modules}

    echo "Modules directory contents:"
    ls -laR $out/modules/
  ''
