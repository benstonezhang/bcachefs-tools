{ inputs, version }:
final: prev:
let
  cBuild = prev.callPackage ./c-build.nix {
    inherit version;
  };
in
{
  bcachefsPackages = {
    "bcachefs-tools" = cBuild.package;
    "bcachefs-tools-fuse" = cBuild.packageFuse;
    "bcachefs-module-linux-latest" =
      final.linuxPackages_latest.callPackage cBuild.package.kernelModule
        { };
    "bcachefs-module-linux-testing" =
      final.linuxPackages_testing.callPackage cBuild.package.kernelModule
        { };
  };
}