# vim: tabstop=2 shiftwidth=2 autoindent expandtab colorcolumn=80
{
  description = "OpenAuto Framework";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-26.05-small";
  };

  outputs = inputs: let
    supportedSystems = [ "x86_64-linux" "aarch64-linux" ];
  in {
    # Lay out outputs explicitly per supported system.
    packages = builtins.listToAttrs (map (system: {
      name = system;
      value = let
        pkgs = inputs.nixpkgs.legacyPackages.${system};
        engine_version = pkgs.lib.trivial.pipe
          "${pkgs.flutter.unwrapped}/bin/internal/engine.version" [
          builtins.readFile
          pkgs.lib.strings.trim
        ];
        flutter_engine_url =
          "https://storage.googleapis.com/flutter_infra_release/flutter/${engine_version}";
        engine_platform =
          if system == "x86_64-linux" then "linux-x64"
          else if system == "aarch64-linux" then "linux-arm64"
          else throw "Unsupported system '${system}'";
        engine_hash =
          if system == "x86_64-linux" then
            "sha256-p42TKkhHZ96FR/6pvoGGaYtqidxl4GhdhjqQuBChLYc="
          else if system == "aarch64-linux" then
            "sha256-dEq0/MnXvxV+n2b9e2tL/7nEGTjJwO2/Wlw2Wz9mm+w="
          else
            throw "Unsupported system '${system}' for the Flutter embedder";

        flutter-engine = pkgs.stdenvNoCC.mkDerivation {
          pname = "flutter-embedder";
          version = engine_version;
          src = pkgs.fetchzip {
            url = "${flutter_engine_url}/${engine_platform}/${engine_platform}-embedder.zip";
            hash = engine_hash;
            stripRoot = false;
          };
          nativeBuildInputs = [ pkgs.flutter ];
          phases = [ "installPhase" ];
          installPhase = ''
            mkdir $out/{lib,include,share} -p

            echo "${engine_version}" > $out/share/engine.version
            echo "${flutter_engine_url}/${engine_platform}/${engine_platform}-embedder.zip" \
              > $out/share/engine.url

            cp $src/LICENSE* $out/share/
            cp $src/*.so -Rf $out/lib/
            cp $src/*.h -Rf $out/include/

            ln -s flutter_embedder.h $out/include/embedder.h
          '';
        };

        # The shell executable, parameterized by the display backend. The
        # backend is chosen at build time (default: wayland).
        shellFor = backend:
          pkgs.stdenv.mkDerivation {
            pname = "openauto-shell";
            version = "0.1.0";
            src = ./shell;
            nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config pkgs.ninja ];
            buildInputs = [
              pkgs.libGL.dev  # EGL + OpenGLES2 headers
              pkgs.wayland    # wayland-client / wayland-egl
              pkgs.libgbm     # GBM (KMS)
              pkgs.libdrm     # DRM (KMS)
              pkgs.libffi
              pkgs.libxkbcommon
              flutter-engine
            ];
            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              "-DCMAKE_INSTALL_PREFIX=${placeholder "out"}"
              "-DFLUTTER_ENGINE_ROOT=${flutter-engine}"
              "-DOA_BACKEND=${backend}"
            ];
          };

        shell = shellFor "wayland";
        shell-kms = shellFor "kms";
      in {
        flutter-engine = flutter-engine;
        default = flutter-engine;
        shell = shell;
        shell-kms = shell-kms;
      };
    }) supportedSystems);

    devShells = builtins.listToAttrs (map (system: {
      name = system;
      value = let
        pkgs = inputs.nixpkgs.legacyPackages.${system};
      in {
        default = pkgs.mkShell {
          packages = [ pkgs.cmake pkgs.pkg-config pkgs.ninja pkgs.flutter ];
          buildInputs = [
            pkgs.libGL.dev
            pkgs.wayland
            pkgs.libgbm
            pkgs.libdrm
            pkgs.libffi
            pkgs.libxkbcommon
            pkgs.flutter.unwrapped
          ];
        };
      };
    }) supportedSystems);
  };
}
