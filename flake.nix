{
  description = "Userspace tools for bcachefs";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

    flake-parts.url = "github:hercules-ci/flake-parts";

    treefmt-nix = {
      url = "github:numtide/treefmt-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    flake-compat = {
      url = "github:edolstra/flake-compat";
      flake = false;
    };

    nix-github-actions = {
      url = "github:nix-community/nix-github-actions";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    inputs@{
      self,
      nixpkgs,
      flake-parts,
      treefmt-nix,
      flake-compat,
      nix-github-actions,
    }:
    let
      # i686-linux dropped: no real consumers, and cross sqlite tcltest
      # tries to run on i686 and fails.
      systems = nixpkgs.lib.filter
        (s: nixpkgs.lib.hasSuffix "-linux" s && s != "i686-linux")
        nixpkgs.lib.systems.flakeExposed;

      # Extract version from Changelog.mdwn (first line starting with "## vX.Y.Z")
      version = let
        changelog = builtins.readFile ./Changelog.mdwn;
        lines = builtins.filter
          (x: builtins.isString x && x != "")
          (builtins.split "\n" changelog);
        findVersion = lines:
          if lines == [] then null
          else let
            m = builtins.match "## v([0-9]+\\.[0-9]+\\.[0-9]+).*" (builtins.head lines);
          in if m != null then builtins.head m else findVersion (builtins.tail lines);
        firstVersion = findVersion lines;
      in
        if firstVersion != null then firstVersion else "0.0.0";

      rev = self.shortRev or self.dirtyShortRev or (nixpkgs.lib.substring 0 8 self.lastModifiedDate);
      fullVersion = "${version}+${rev}";
    in
    flake-parts.lib.mkFlake { inherit inputs; } {
      imports = [ inputs.treefmt-nix.flakeModule ];

      flake = {
        githubActions = nix-github-actions.lib.mkGithubMatrix {
          # github actions supports fewer architectures
          checks = nixpkgs.lib.getAttrs [ "aarch64-linux" "x86_64-linux" ] self.checks;
        };
        nixosModules = let
          bcachefsNixosModule = { pkgs, ... }: {
            boot.supportedFilesystems = [ "bcachefs" ];
            boot.bcachefs.package =
              (pkgs.extend self.overlays.default).bcachefsPackages.bcachefs-tools;
          };
        in {
          default = bcachefsNixosModule;
          bcachefs = bcachefsNixosModule;
        };
      };

      inherit systems;

      flake.overlays.default = import ./overlay.nix { inherit inputs version; };

      perSystem =
        {
          self',
          config,
          lib,
          system,
          ...
        }:
        let
          pkgs = import nixpkgs {
            inherit system;
            overlays = [ self.overlays.default ];
          };
          latexDerivation = (
            pkgs.texliveBasic.withPackages (
              ps: with ps; [
                imakeidx
                xkeyval
                upquote
                collection-fontsrecommended
              ]
            )
          );
        in
        {
          packages =
            let
              packagesForSystem =
                crossSystem:
                let
                  localSystem = system;
                  pkgs' = import nixpkgs {
                    inherit crossSystem localSystem;
                    overlays = [ self.overlays.default ];
                  };

                  withCrossName =
                    set: lib.mapAttrs' (name: value: lib.nameValuePair "${name}-${crossSystem}" value) set;
                in
                (withCrossName pkgs'.bcachefsPackages)
                // lib.optionalAttrs (crossSystem == localSystem) pkgs'.bcachefsPackages;
              packages = lib.mergeAttrsList (map packagesForSystem systems);
            in
            packages
            // {
              default = self'.packages.bcachefs-tools;
              doc = pkgs.stdenv.mkDerivation {
                pname = "bcachefs-tools-doc";
                version = fullVersion;
                src = ./doc;
                buildInputs = with pkgs; [
                  latexDerivation
                ];
                buildPhase = ''
                  pdflatex bcachefs-principles-of-operation.tex
                  pdflatex bcachefs-principles-of-operation.tex
                '';
                installPhase = ''
                  mkdir -p $out/doc
                  cp bcachefs-principles-of-operation.pdf $out/doc
                '';
              };
            };

          checks =
            let
              nativeChecks = lib.filterAttrs
                (_: v: v != null)
                (lib.genAttrs
                  [ "bcachefs-tools" "bcachefs-tools-fuse"
                    "bcachefs-module-linux-latest" "bcachefs-module-linux-testing"
                  ]
                  (name: self'.packages.${name} or null));
            in
            nativeChecks;

          devShells.default = pkgs.mkShell {
            inputsFrom = [
              config.treefmt.build.devShell
              self'.packages.default
            ];

            packages = with pkgs; [
              bear
              clang-tools
              gdb
              valgrind
            ];
          };

          devShells.doc = pkgs.mkShell {
            packages = with pkgs; [
              latexDerivation
            ];
          };

          treefmt.config = {
            projectRootFile = "flake.nix";
            flakeCheck = false;

            programs = {
              nixfmt.enable = true;
              clang-format.enable = true;
            };
          };
        };
    };
}