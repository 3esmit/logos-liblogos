# Builds everything - shared build derivation
{ pkgs, common, src }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-build";
  # qtbase's setup hook errors in qtPreHook unless a wrapper hook ran or this is
  # set; the wrapper hooks are absent on Windows and would skip a PE anyway.
  dontWrapQtApps = true;
  version = common.version;

  inherit src;
  inherit (common) nativeBuildInputs buildInputs cmakeFlags meta env;
  
  # Build everything but don't install yet
  # The install is done by the component-specific derivations
}

