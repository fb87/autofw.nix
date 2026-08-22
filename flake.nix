# vim: tabstop=2 shiftwidth=2 autoindent expandtab colorcolumn=80
{
  description = "OpenAuto Framework";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-26.05-small";
  };

  outputs = inputs: {
    packages = builtins.mapAttrs (system: pkgs: let
      engine_version = pkgs.lib.trivial.pipe 
        "${pkgs.flutter.unwrapped}/bin/internal/engine.version" [
        builtins.readFile
        pkgs.lib.strings.trim
      ];

      flutter_engine_url = "https://storage.googleapis.com/flutter_infra_release/flutter/${engine_version}";

      flutter_engine = pkgs.stdenvNoCC.mkDerivation {
        pname = "flutter-embedder";
      	version = engine_version;

	      src = pkgs.fetchzip {
	        url = "${flutter_engine_url}/linux-x64/linux-x64-embedder.zip";
	        hash = "sha256-p42TKkhHZ96FR/6pvoGGaYtqidxl4GhdhjqQuBChLYc=";
	        stripRoot = false;
	      };

	      nativeBuildInputs = [ pkgs.flutter ];

	      phases = [ "installPhase" ];

	      installPhase = ''
       	  mkdir $out/{lib,include,share} -p

	        echo "${engine_version}" > $out/share/engine.version
	        echo "${flutter_engine_url}/linux-x64/linux-x64-embedder.zip" \
            > $out/share/engine.url

	        cp $src/LICENSE* $out/share/
	        cp $src/*.so -Rf $out/lib/
	        cp $src/*.h -Rf $out/include/

          ln -s flutter_embedder.h $out/include/embedder.h
	      '';
      };
    in {
      flutter-engine = flutter_engine;
      default = inputs.self.packages.${system}.flutter-engine;
    }) inputs.nixpkgs.legacyPackages;
  };
}
