{
  lib,
  pkgs,
  stdenv,

  # build time
  pkg-config,
  versionCheckHook,

  # run time
  fuse3,
  keyutils,
  libaio,
  libsodium,
  libunwind,
  liburcu,
  libuuid,
  lz4,
  udev,
  zlib,
  zstd,

  version,
}:
let
  args = {
    src = lib.fileset.toSource {
      root = ./.;
      fileset = lib.fileset.fileFilter ({ hasExt, ... }: !hasExt "nix") ./.;
    };
    strictDeps = true;

    env = {
      PKG_CONFIG_SYSTEMD_SYSTEMDSYSTEMUNITDIR = "${placeholder "out"}/lib/systemd/system";
      PKG_CONFIG_UDEV_UDEVDIR = "${placeholder "out"}/lib/udev";
    };

    makeFlags = [
      "INITRAMFS_DIR=${placeholder "out"}/etc/initramfs-tools"
      "PREFIX=${placeholder "out"}"
      "VERSION=${version}"
    ];

    dontStrip = true;

    nativeBuildInputs = [
      pkg-config
    ];

    buildInputs = [
      keyutils
      libaio
      libsodium
      libunwind
      liburcu
      libuuid
      lz4
      udev
      zlib
      zstd
    ];

    enableParallelBuilding = true;
    buildPhase = ''
      runHook preBuild
      make ''${enableParallelBuilding:+-j''${NIX_BUILD_CORES}} $makeFlags
      runHook postBuild
    '';

    enableParallelInstalling = true;
    installPhase = ''
      runHook preInstall
      make ''${enableParallelInstalling:+-j''${NIX_BUILD_CORES}} $makeFlags install install_dkms
      runHook postInstall
    '';

    doInstallCheck = true;
    nativeInstallCheckInputs = [ versionCheckHook ];
    versionCheckProgramArg = "version";
  };

package = stdenv.mkDerivation (
    args
    // {
      pname = "bcachefs-tools";
      version = toString version;
      outputs = [
        "out"
        "dkms"
      ];

      makeFlags = args.makeFlags ++ [
        "DKMSDIR=${placeholder "dkms"}"
      ];

      passthru.kernelModule = import ./module-build.nix package;

      meta = {
        description = "Userspace tools for bcachefs";
        license = lib.licenses.gpl2Only;
        mainProgram = "bcachefs";
      };
    }
  );

  packageFuse = package.overrideAttrs (
    final: prev: {
      __intentionallyOverridingVersion = true;
      pname = "bcachefs-tools-fuse";
      version = toString version;
      makeFlags = prev.makeFlags ++ [ "BCACHEFS_FUSE=1" ];
      buildInputs = prev.buildInputs ++ [ fuse3 ];
    }
  );
in
{
  inherit
    package
    packageFuse
  ;
}