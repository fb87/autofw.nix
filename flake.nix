# vim: tabstop=2 shiftwidth=2 autoindent expandtab colorcolumn=80
{
  description = "OpenAuto Framework";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-26.05-small";
  };

  outputs = { self, ... } @ inputs: let
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

        # Seconds after which `nix run .#channel-demo` stops the demo app.
        demoTimeout = 60;

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

        # The shell app, parameterized by display backend and renderer.
        shellFor = backend: renderer:
          pkgs.stdenv.mkDerivation {
            pname = "shell-${backend}-${renderer}";
            version = "0.1.0";
            src = ./.;
            nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config pkgs.ninja ];
            buildInputs = [
              pkgs.libGL.dev
              pkgs.wayland
              pkgs.libdrm
              pkgs.libgbm
              pkgs.libffi
              pkgs.libxkbcommon
              flutter-engine
            ];
            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              "-DCMAKE_INSTALL_PREFIX=${placeholder "out"}"
              "-DFLUTTER_ENGINE_ROOT=${flutter-engine}"
              "-DOA_BACKEND=${backend}"
              "-DOA_RENDERER=${renderer}"
            ];
          };

        shell = shellFor "wayland" "gl";
        shell-wayland-software = shellFor "wayland" "software";
        shell-drm = shellFor "drm" "gl";
        shell-drm-software = shellFor "drm" "software";
        # Prebuilt Flutter assets for the channel demo (kernel_blob.bin, icons,
        # shaders and icudtl.dat). Built once offline (see examples/channel_demo)
        # and checked into assets-bundle/ so the sandboxed build stays offline.
        demoAssets = pkgs.runCommandLocal "channel-demo-assets" {} ''
          mkdir -p $out
          cp -r ${./examples/channel_demo/assets-bundle}/* $out/
        '';

        # A self-contained `nix run` target: the Wayland/GL shell driving the
        # channel demo assets, bounded by a timeout so it never runs forever
        # on a headless/CI box. Override with `nix run .#channel-demo -- 300`.
        channel-demo = pkgs.writeShellScriptBin "channel-demo" ''
          timeout_s="${toString demoTimeout}"
          if [ "$#" -gt 0 ] && [ "$1" -ge 0 ] 2>/dev/null; then
            timeout_s="$1"
            shift
          fi
          exec ${pkgs.coreutils}/bin/timeout "$timeout_s" \
            ${shell}/bin/shell_app \
            --assets=${demoAssets} \
            --icu=${demoAssets}/icudtl.dat \
            --renderer=gl "$@"
        '';
      in {
        flutter-engine = flutter-engine;
        default = flutter-engine;
        inherit shell shell-wayland-software shell-drm shell-drm-software;
        inherit channel-demo demoAssets;
      };
    }) supportedSystems);

    apps = builtins.listToAttrs (map (system: {
      name = system;
      value = {
        channel-demo = {
          type = "app";
          program = "${self.packages.${system}.channel-demo}/bin/channel-demo";
        };
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
