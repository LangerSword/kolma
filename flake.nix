{
  description = "kolma -- a pluggable compression engine with an analyser, a parallel chunk engine and a terminal interface";

  # NOTE: this flake is provided as a reproducible build definition, but it has
  # NOT been exercised on the machine where the project was written (Nix was not
  # installed there). The CMake build and the Go module build are the tested
  # paths; treat the Nix expression as a starting point and expect to adjust it.

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        version = "0.1.0";

        engine = pkgs.stdenv.mkDerivation {
          pname = "kolma";
          inherit version;
          src = ./.;
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DKOLMA_WARNINGS_AS_ERRORS=ON"
          ];
          doCheck = true;
          checkPhase = ''
            runHook preCheck
            ./kolma_tests
            runHook postCheck
          '';
          meta = {
            description = "Pluggable compression engine";
            license = pkgs.lib.licenses.mit;
            mainProgram = "kolma";
          };
        };

        tui = pkgs.buildGoModule {
          pname = "kolma-tui";
          inherit version;
          src = ./tui;
          vendorHash = null;
          ldflags = [ "-s" "-w" ];
          postInstall = ''
            mv $out/bin/tui $out/bin/kolma-tui
          '';
          meta = {
            description = "Terminal interface for the kolma compression engine";
            license = pkgs.lib.licenses.mit;
            mainProgram = "kolma-tui";
          };
        };
      in {
        packages = {
          inherit engine tui;
          default = engine;
        };

        apps = {
          kolma = flake-utils.lib.mkApp { drv = engine; };
          kolma-tui = flake-utils.lib.mkApp {
            drv = tui;
            # The TUI is a client: it needs the engine on PATH.
            exePath = "/bin/kolma-tui";
          };
          default = flake-utils.lib.mkApp { drv = engine; };
        };

        devShells.default = pkgs.mkShell {
          packages = [
            pkgs.cmake
            pkgs.ninja
            pkgs.gcc
            pkgs.clang-tools
            pkgs.go
            pkgs.python3
          ];
          shellHook = ''
            echo "kolma dev shell"
            echo "  make check    build everything and run every test"
          '';
        };
      });
}