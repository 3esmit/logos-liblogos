# Extracts libraries from the shared build
{ pkgs, common, build }:

let
  # Extract the package-manager root from CMake flags
  logosPackageManagerRoot = common.env.LOGOS_PACKAGE_MANAGER_ROOT;
  # The shared runtime liblogos_core now IMPORTS rather than defines.
  logosProtocolRoot = common.env.LOGOS_PROTOCOL_ROOT;
  logosQtHostRoot = common.env.LOGOS_QT_HOST_ROOT;
in
pkgs.runCommand "${common.pname}-lib-${common.version}"
  {
    inherit (common) meta;
  }
  ''
    # Copy libraries from the shared build
    mkdir -p $out/lib
    if [ -d ${build}/lib ]; then
      cp -r ${build}/lib/* $out/lib/
    fi

    # Fail loudly rather than shipping a lib output with no RUNTIME artifact.
    # Verified failure this guards against: drop `RUNTIME DESTINATION lib` from
    # install(TARGETS logos_core) and the Windows build still exits 0 while
    # liblogos_core.dll appears nowhere in the package -- only the
    # liblogos_core.dll.a import library does. Match the loadable image
    # explicitly; a `liblogos_core.*` glob is satisfied by the import library
    # and catches nothing.
    #
    # Tested explicitly with `for`/`-f` rather than a glob array: bash's
    # nullglob only drops patterns that CONTAIN a wildcard, so a literal
    # "$out/lib/liblogos_core.dll" survives into the array even when the file
    # does not exist and the check passes vacuously. (Observed: the first
    # version of this guard did exactly that and let the broken build through.)
    found=""
    for cand in $out/lib/liblogos_core.so $out/lib/liblogos_core.dylib $out/lib/liblogos_core.dll; do
      [ -f "$cand" ] && found="$cand"
    done
    if [ -z "$found" ]; then
      echo "Error: no loadable logos_core library in ${build}/lib" >&2
      ls -la ${build}/lib ${build}/bin >&2 || true
      exit 1
    fi

    # Bundle package_manager_lib alongside logos_core (logos_core links against
    # it), and liblgx, which logos_core now links DIRECTLY for the dependency
    # gate's semver. Before that, liblgx was reached only through the absolute
    # store path baked into libpackage_manager_lib; now liblogos_core records
    # @rpath/liblgx and its only LC_RPATH is @loader_path, so it must be here.
    for f in ${logosPackageManagerRoot}/lib/libpackage_manager_lib* \
             ${logosPackageManagerRoot}/lib/liblgx.so* \
             ${logosPackageManagerRoot}/lib/liblgx.dylib; do
      if [ -f "$f" ]; then
        cp -L "$f" $out/lib/
      fi
    done

    # The shared C++ runtime, which liblogos_core now IMPORTS instead of
    # absorbing. This is NOT optional packaging: liblogos_core records
    # @rpath/liblogos_qt_host.dylib and @rpath/liblogos_protocol.dylib, and its
    # only LC_RPATH is @loader_path -- so the loader looks for them BESIDE
    # itself, i.e. in this directory, and nowhere else.
    #
    # Measured before this existed: the build succeeded, `logos_host --help`
    # exited 0, and dlopen of the library failed outright with
    #   Library not loaded: @rpath/liblogos_qt_host.dylib
    # A help-text smoke test does not touch the library, so nothing in the build
    # or in a boot check would have caught it.
    #
    # Explicit `for` + `-f` rather than a glob array, for the reason spelled out
    # in the Windows block below: a fully interpolated literal path survives into
    # an array even under nullglob, so a guard over it passes vacuously.
    for f in ${logosProtocolRoot}/lib/liblogos_protocol.so* \
             ${logosProtocolRoot}/lib/liblogos_protocol.dylib \
             ${logosProtocolRoot}/bin/liblogos_protocol.dll \
             ${logosQtHostRoot}/lib/liblogos_qt_host.so* \
             ${logosQtHostRoot}/lib/liblogos_qt_host.dylib \
             ${logosQtHostRoot}/bin/liblogos_qt_host.dll; do
      if [ -f "$f" ]; then
        cp -L "$f" $out/lib/
      fi
    done

    # Assert it, rather than trusting the copy above. The failure mode is a
    # library that builds and installs and cannot be loaded.
    _shared_found=0
    for f in $out/lib/liblogos_protocol.* $out/lib/liblogos_qt_host.*; do
      [ -f "$f" ] && _shared_found=$((_shared_found + 1))
    done
    if [ "$_shared_found" -lt 2 ]; then
      echo "ERROR: liblogos_core imports the shared runtime, but only $_shared_found" >&2
      echo "       of liblogos_protocol / liblogos_qt_host were staged into \$out/lib." >&2
      echo "       liblogos_core resolves them through @loader_path and will fail to load." >&2
      exit 1
    fi

    ${pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isWindows ''
      # Windows only: libpackage_manager_lib's OWN dependency, liblgx.
      #
      # An ELF or Mach-O consumer never needs this -- the copied
      # libpackage_manager_lib carries an RPATH/install-name pointing back at
      # the store path liblgx lives in, so the loader finds it there. PE has no
      # rpath: an import table carries the DLL BASE NAME and Windows resolves
      # it from the loading executable's directory. Whatever bundles this lib
      # output can only stage what is IN it, so a dependency left behind here
      # is a dependency that cannot be staged later.
      #
      # Measured, and the reason this exists: logosctl.exe with every other DLL
      # correctly beside it exited 53 with NO OUTPUT AT ALL -- the loader
      # failing on liblgx.dll before main() ran. Nothing in the build, the
      # package, or the run says which DLL is missing.
      #
      # Every DLL the package-manager output carries, not just liblgx: liblgx
      # has its own imports (libsodium, ICU, zlib), and package-manager stages
      # that whole closure for exactly this reason. Copying the set rather than
      # naming members keeps it transitive instead of a list that rots.
      #
      # Explicit `for` + `-f` rather than a nullglob array: nullglob only drops
      # patterns that CONTAIN a wildcard, so a fully interpolated literal path
      # survives into the array and a guard over it passes vacuously.
      lgx=0
      for f in ${logosPackageManagerRoot}/lib/*.dll ${logosPackageManagerRoot}/lib/*.dll.a; do
        [ -f "$f" ] || continue
        [ -e "$out/lib/$(basename "$f")" ] && continue
        cp -L "$f" $out/lib/
        case "$(basename "$f")" in liblgx.dll) lgx=$((lgx + 1));; esac
      done
      if [ "$lgx" -eq 0 ] && [ ! -f "$out/lib/liblgx.dll" ]; then
        echo "Error: liblgx.dll not found under ${logosPackageManagerRoot}/lib;" >&2
        echo "       libpackage_manager_lib.dll imports it, so any .exe loading" >&2
        echo "       liblogos_core.dll would fail with no diagnostic." >&2
        ls -la ${logosPackageManagerRoot}/lib >&2 || true
        exit 1
      fi
    ''}
  ''
