# Bundles modules from external flake inputs into the logos_host_qt modules directory.
# logos_host_qt expects: modules/<name>/manifest.json + <name>_plugin.{so,dylib,dll}
# When portableBuild is false (default/dev), manifest keys get a "-dev" suffix
# to match the dev variant lookup in platformVariantsToTry().
{ pkgs, common, capabilityModule, portableBuild ? false }:

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
in
pkgs.runCommand "${common.pname}-modules-${common.version}"
  {
    inherit (common) meta;
  }
  ''
    mkdir -p $out/modules/capability_module

    # Copy the plugin library. Every extension is listed explicitly -- an
    # if/elif chain, or a Unix-only pair of globs, installs NOTHING on Windows
    # and still succeeds.
    shopt -s nullglob
    plugins=(${capabilityModule}/lib/*.dylib ${capabilityModule}/lib/*.so ${capabilityModule}/lib/*.dll)
    for lib in "''${plugins[@]}"; do
      cp "$lib" $out/modules/capability_module/
    done

    # Determine the plugin filename that was copied
    pluginFile=""
    for f in $out/modules/capability_module/*; do
      if [ -f "$f" ]; then
        pluginFile="$(basename "$f")"
        break
      fi
    done

    if [ -z "$pluginFile" ]; then
      echo "Error: No capability_module library found under ${capabilityModule}/lib" >&2
      ls -la ${capabilityModule}/lib >&2 || true
      exit 1
    fi

    platform="${platform}"
    arch="${arch}"
    suffix="${suffix}"

    # Create manifest.json for plugin discovery
    # "type" is REQUIRED, not decorative. PackageManagerLib::getInstalledModules
    # enumerates manifests with `types = {"core"}` and skips -- silently, with
    # no diagnostic -- every manifest whose type does not match. Without this
    # field the directory scans clean, the module never enters the registry,
    # and the only symptom is a later "Module not found in known modules:
    # capability_module" from whoever tried to load it. It matches the "core"
    # in logos-capability-module/metadata.json, which is what the lgx-bundled
    # manifest carries.
    cat > $out/modules/capability_module/manifest.json <<EOF
    {
      "name": "capability_module",
      "version": "1.0.0",
      "type": "core",
      "main": {
        "$platform-$arch$suffix": "$pluginFile",
        "$platform-amd64$suffix": "$pluginFile",
        "$platform-arm64$suffix": "$pluginFile",
        "$platform-x86_64$suffix": "$pluginFile",
        "$platform-aarch64$suffix": "$pluginFile"
      }
    }
    EOF

    echo "Modules directory contents:"
    ls -laR $out/modules/
  ''
