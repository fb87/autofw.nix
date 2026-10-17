# vim: tabstop=2 shiftwidth=2 autoindent expandtab colorcolumn=80
{
  description = "OpenAuto Framework";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-26.05-small";
  };

  outputs = { self, ... } @ inputs: let
    supportedSystems = [ "x86_64-linux" "aarch64-linux" ];

    # Hash of the per-system `<platform>-embedder.zip` (JIT-capable, debug)
    # Flutter engine artifact, keyed by the *target* system. Shared by the
    # native per-system packages below and by the x86_64-linux -> aarch64
    # cross-compiled outputs, which fetch the aarch64 artifact while
    # building entirely on x86_64 (fetch+unzip, no emulation needed).
    engineArtifactHash = {
      x86_64-linux = "sha256-p42TKkhHZ96FR/6pvoGGaYtqidxl4GhdhjqQuBChLYc=";
      aarch64-linux = "sha256-dEq0/MnXvxV+n2b9e2tL/7nEGTjJwO2/Wlw2Wz9mm+w=";
    };

    # A release-mode (AOT-capable) aarch64 libflutter_engine.so, with visible
    # (non-hidden) embedder ABI symbols. Google's own generic embedder.zip is
    # JIT-only (verified empirically: FlutterEngineCreateAOTData refuses with
    # "AOT data can only be created in AOT mode"), and its flavored Linux
    # engine builds (linux-arm64-release/libflutter_linux_gtk.so) hide the
    # embedder C ABI entirely. This pin instead uses
    # https://github.com/ardera/flutter-engine-binaries-for-arm, a
    # community-maintained project that builds the engine from source
    # specifically to keep the embedder ABI visible for custom (non-GTK)
    # Linux embedders such as this one.
    #
    # The commit must stay paired with a matching Flutter SDK (see
    # examples/gallery/scripts/) -- the Dart AOT snapshot format is coupled
    # to the exact engine build. Pinned commit targets Flutter 3.47.7
    # (engine commit deb287481e3ce9468f3434937ced4240a70539ca, confirmed
    # against examples/gallery/scripts/fetch-release-toolchain.sh's download).
    arderaReleaseEnginePin = {
      commit = "054a6d0328c72e8e2e70f3e092ea5ba91b3cf8fd";
      engineSoHash = "sha256-ZbnZtX+6SeTgtXwYwGtt9lwbfyJSUBNDbZyJAfJzerM=";
      icudtlHash = "sha256-n0jH+cfJTVFqFIcHB+kQq5TXWuZA/2hCxK9TJ2zSbr4=";
      embedderHeaderHash = "sha256-lBIkabJUqTI5S7W7XGJTF0Ju79biPUNm4yNyxwrFvjc=";
    };
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
        engine_hash = engineArtifactHash.${system}
          or (throw "Unsupported system '${system}' for the Flutter embedder");

        # Seconds after which `nix run .#gallery` stops the gallery app.
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
              pkgs.libinput
              pkgs.udev
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
        # Prebuilt Flutter assets for the shell gallery (kernel_blob.bin, icons,
        # shaders and icudtl.dat). Built once offline (see examples/gallery)
        # and checked into assets-bundle/ so the sandboxed build stays offline.
        galleryAssets = pkgs.runCommandLocal "shell-gallery-assets" {} ''
          mkdir -p $out
          cp -r ${./examples/gallery/assets-bundle}/* $out/
        '';

        # A self-contained `nix run` target: the Wayland/GL shell driving the
        # gallery assets, bounded by a timeout so it never runs forever on a
        # headless/CI box. Override with `nix run .#gallery -- 300`.
        gallery = pkgs.writeShellScriptBin "gallery" ''
          timeout_s="${toString demoTimeout}"
          if [ "$#" -gt 0 ] && [ "$1" -ge 0 ] 2>/dev/null; then
            timeout_s="$1"
            shift
          fi
          exec ${pkgs.coreutils}/bin/timeout "$timeout_s" \
            ${shell}/bin/shell_app \
            --assets=${galleryAssets} \
            --icu=${galleryAssets}/icudtl.dat \
            --renderer=gl "$@"
        '';

        # -- true cross-compilation to aarch64, built entirely on x86_64 --
        #
        # Unlike `packages.aarch64-linux.*` above (native `legacyPackages`,
        # which need an aarch64 builder or emulation), these use
        # `pkgsCross.aarch64-multiplatform`: the derivation's build platform
        # stays x86_64-linux (real cross toolchain, no emulation to compile)
        # while the output is an aarch64 ELF.
        crossOutputs = pkgs.lib.optionalAttrs (system == "x86_64-linux") (let
          pkgsCrossAarch64 = pkgs.pkgsCross.aarch64-multiplatform;

          # Same shape as `flutter-engine` above, targeting linux-arm64, but
          # built with the *native* stdenvNoCC so fetching+unzipping the
          # artifact doesn't itself need an aarch64 builder/emulation.
          flutter-engine-cross-aarch64 = pkgs.stdenvNoCC.mkDerivation {
            pname = "flutter-embedder-cross-aarch64";
            version = engine_version;
            src = pkgs.fetchzip {
              url = "${flutter_engine_url}/linux-arm64/linux-arm64-embedder.zip";
              hash = engineArtifactHash.aarch64-linux;
              stripRoot = false;
            };
            phases = [ "installPhase" ];
            installPhase = ''
              mkdir $out/{lib,include,share} -p
              cp $src/LICENSE* $out/share/
              cp $src/*.so -Rf $out/lib/
              cp $src/*.h -Rf $out/include/
              ln -s flutter_embedder.h $out/include/embedder.h
            '';
          };

          # Release/AOT-capable counterpart (see arderaReleaseEnginePin
          # above). Also built with the native stdenvNoCC: it's just pinned
          # single-file fetches, no compilation.
          flutter-engine-release-cross-aarch64 = let
            base = "https://raw.githubusercontent.com/ardera/"
              + "flutter-engine-binaries-for-arm/${arderaReleaseEnginePin.commit}";
            engineLib = pkgs.fetchurl {
              url = "${base}/arm64/libflutter_engine.so.release";
              hash = arderaReleaseEnginePin.engineSoHash;
            };
            icudtl = pkgs.fetchurl {
              url = "${base}/arm64/icudtl.dat";
              hash = arderaReleaseEnginePin.icudtlHash;
            };
            embedderHeader = pkgs.fetchurl {
              url = "${base}/flutter_embedder.h";
              hash = arderaReleaseEnginePin.embedderHeaderHash;
            };
          in pkgs.stdenvNoCC.mkDerivation {
            pname = "flutter-embedder-release-cross-aarch64";
            version = arderaReleaseEnginePin.commit;
            phases = [ "installPhase" ];
            installPhase = ''
              mkdir $out/{lib,include,share} -p
              cp ${engineLib} $out/lib/libflutter_engine.so
              cp ${embedderHeader} $out/include/flutter_embedder.h
              ln -s flutter_embedder.h $out/include/embedder.h
              cp ${icudtl} $out/share/icudtl.dat
              echo "${arderaReleaseEnginePin.commit}" > $out/share/ardera-commit.version
            '';
          };

          shellForCross = engineRoot: backend: renderer:
            pkgsCrossAarch64.stdenv.mkDerivation {
              pname = "shell-aarch64-${backend}-${renderer}";
              version = "0.1.0";
              src = ./.;
              nativeBuildInputs = [
                pkgsCrossAarch64.cmake pkgsCrossAarch64.pkg-config pkgsCrossAarch64.ninja
              ];
              buildInputs = [
                pkgsCrossAarch64.libGL.dev
                pkgsCrossAarch64.wayland
                pkgsCrossAarch64.libdrm
                pkgsCrossAarch64.libgbm
                pkgsCrossAarch64.libffi
                pkgsCrossAarch64.libxkbcommon
                pkgsCrossAarch64.libinput
                pkgsCrossAarch64.udev
                engineRoot
              ];
              cmakeFlags = [
                "-DCMAKE_BUILD_TYPE=Release"
                "-DCMAKE_INSTALL_PREFIX=${placeholder "out"}"
                "-DFLUTTER_ENGINE_ROOT=${engineRoot}"
                "-DOA_BACKEND=${backend}"
                "-DOA_RENDERER=${renderer}"
              ];
            };
        in {
          inherit flutter-engine-cross-aarch64 flutter-engine-release-cross-aarch64;
          shell-aarch64 = shellForCross flutter-engine-cross-aarch64 "wayland" "gl";
          shell-aarch64-wayland-software =
            shellForCross flutter-engine-cross-aarch64 "wayland" "software";
          shell-aarch64-drm = shellForCross flutter-engine-cross-aarch64 "drm" "gl";
          shell-aarch64-drm-software =
            shellForCross flutter-engine-cross-aarch64 "drm" "software";
          shell-aarch64-release =
            shellForCross flutter-engine-release-cross-aarch64 "wayland" "gl";
          shell-aarch64-release-drm =
            shellForCross flutter-engine-release-cross-aarch64 "drm" "gl";
        });
      in {
        flutter-engine = flutter-engine;
        default = flutter-engine;
        inherit shell shell-wayland-software shell-drm shell-drm-software;
        inherit gallery galleryAssets;
      } // crossOutputs;
    }) supportedSystems);

    apps = builtins.listToAttrs (map (system: {
      name = system;
      value = {
        gallery = {
          type = "app";
          program = "${self.packages.${system}.gallery}/bin/gallery";
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
            pkgs.libinput
            pkgs.udev
            pkgs.flutter.unwrapped
          ];
        };
      };
    }) supportedSystems);
  };
}
