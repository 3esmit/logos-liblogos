# Builds tests
{ pkgs, common, build }:

# The suite is POSIX-only (posix_spawn/waitpid/kill, /bin/sh) and CMake turns
# LOGOS_BUILD_TESTS off for a Windows host, so there would be no
# `logos_core_tests` target to build. Refuse loudly instead: the configurePhase
# below hand-rolls its `cmake` invocation and never expands $cmakeFlags, so a
# Windows instantiation would silently drop -DCMAKE_SYSTEM_NAME=Windows and
# every entry of logosQtCrossCmakeFlags -- i.e. configure as a NATIVE build and
# link the wrong architecture, which is far worse than an error. flake.nix
# already withholds this attribute on Windows; this makes that non-negotiable.
if pkgs.stdenv.hostPlatform.isWindows then
  throw "logos-liblogos: the logos_core test suite is POSIX-only and cannot be cross-compiled for ${pkgs.stdenv.hostPlatform.system}"
else

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-tests";
  version = common.version;
  
  inherit (build) src;
  inherit (common) buildInputs meta env;
  
  # Add platform-specific tools conditionally
  nativeBuildInputs = common.nativeBuildInputs 
    ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ]
    ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.patchelf ];
  
  # Use the same CMake flags as the main build
  cmakeFlags = common.cmakeFlags;
  
  # Configure phase - reuse build outputs
  configurePhase = ''
    runHook preConfigure
    
    # Copy the built artifacts from the main build
    cp -r ${build}/* .
    chmod -R u+w .
    
    # Reconfigure to generate test targets
    cmake -B build -S ${build.src} \
      -GNinja \
      -DLOGOS_CPP_SDK_ROOT=${common.env.LOGOS_CPP_SDK_ROOT} \
      -DLOGOS_PROTOCOL_ROOT=${common.env.LOGOS_PROTOCOL_ROOT} \
      -DLOGOS_QT_SDK_ROOT=${common.env.LOGOS_QT_SDK_ROOT} \
      -DLOGOS_MODULE_ROOT=${common.env.LOGOS_MODULE_ROOT} \
      -DPROCESS_STATS_ROOT=${common.env.PROCESS_STATS_ROOT} \
      -DLOGOS_CONTAINER_ROOT=${common.env.LOGOS_CONTAINER_ROOT} \
      -DLOGOS_MODULE_LOADER_ROOT=${common.env.LOGOS_MODULE_LOADER_ROOT} \
      -DLOGOS_PACKAGE_MANAGER_ROOT=${common.env.LOGOS_PACKAGE_MANAGER_ROOT} \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$out
    
    runHook postConfigure
  '';
  
  # Build phase - build the test executable
  buildPhase = ''
    runHook preBuild
    
    cd build
    ninja logos_core_tests
    
    runHook postBuild
  '';
  
  # Install phase - install the test executable
  installPhase = ''
    runHook preInstall
    
    mkdir -p $out/bin
    cp bin/logos_core_tests $out/bin/
    
    # Copy the libraries so tests can run
    mkdir -p $out/lib
    cp -r lib/* $out/lib/ || true
    # Copy package_manager_lib from its nix store path
    cp ${common.env.LOGOS_PACKAGE_MANAGER_ROOT}/lib/libpackage_manager_lib.* $out/lib/ || true

    ${pkgs.lib.optionalString pkgs.stdenv.isDarwin ''
      # Fix RPATH to find libraries in $out/lib on macOS
      install_name_tool \
        -change @rpath/liblogos_core.dylib $out/lib/liblogos_core.dylib \
        $out/bin/logos_core_tests || true
    ''}

    ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
      # Fix RPATH on Linux to avoid /build/ references and include all dependencies.
      # spdlog links libfmt; both must be on RPATH because patchelf replaces the default search paths.
      # OpenSSL (libssl, libcrypto) is needed because the SDK's plain-C++ TLS
      # transport links it transitively — without this the wrapped binary
      # dies with `libssl.so.3: cannot open shared object`.
      _rpath="$out/lib:${pkgs.boost}/lib:${common.env.LOGOS_PACKAGE_MANAGER_ROOT}/lib:${pkgs.gtest}/lib:${pkgs.qt6.qtbase}/lib:${pkgs.qt6.qtremoteobjects}/lib:${pkgs.spdlog}/lib:${pkgs.fmt}/lib:${pkgs.openssl.out}/lib:${pkgs.stdenv.cc.cc.lib}/lib"
      patchelf --set-rpath "$_rpath" $out/bin/logos_core_tests || true
      # Fix RPATH on liblogos_core.so so it can find its transitive deps (e.g. libboost_process, spdlog, fmt, libssl)
      _rpath_lib="$out/lib:${pkgs.boost}/lib:${common.env.LOGOS_PACKAGE_MANAGER_ROOT}/lib:${pkgs.qt6.qtbase}/lib:${pkgs.qt6.qtremoteobjects}/lib:${pkgs.spdlog}/lib:${pkgs.fmt}/lib:${pkgs.openssl.out}/lib:${pkgs.stdenv.cc.cc.lib}/lib"
      patchelf --set-rpath "$_rpath_lib" $out/lib/liblogos_core.so || true
    ''}
    
    runHook postInstall
  '';
}
