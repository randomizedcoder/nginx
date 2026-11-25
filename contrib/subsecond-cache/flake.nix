{
  description = "nginx millisecond cache reference experiment";
  inputs.nixpkgs.url = "https://channels.nixos.org/nixpkgs-unstable/nixexprs.tar.zst";

  outputs = { self, nixpkgs }:
    let
      systems = [ "aarch64-darwin" "x86_64-linux" "aarch64-linux" ];
      each = f: nixpkgs.lib.genAttrs systems (system: f system (perSystem system));
      perSystem = system: let
      pkgs = nixpkgs.legacyPackages.${system};
      revision = "2e8440bcdc438f0f1cd03da778a3843ddb3c86d9";
      build = name: src: pkgs.stdenv.mkDerivation {
        pname = name;
        version = "experiment";
        inherit src;
        nativeBuildInputs = [ pkgs.perl ];
        buildInputs = [ pkgs.pcre2 pkgs.zlib ];
        configurePhase = ''
          runHook preConfigure
          ./auto/configure --prefix="$out"
          runHook postConfigure
        '';
        enableParallelBuilding = true;
      };
      patched = build "nginx-fine" ../..;
      baseline = build "nginx-baseline" (builtins.fetchGit {
        url = "https://github.com/randomizedcoder/nginx.git";
        rev = revision;
      });
      harness = builtins.fetchGit {
        url = "https://github.com/nginx/nginx-tests.git";
        rev = "4d1de3f357afeb91865d024b27516d6b67aad86e";
      };
      experiment = name: pkgs.writeShellApplication {
        inherit name;
        runtimeInputs = [ pkgs.python3 pkgs.ffmpeg-headless pkgs.wrk ];
        text = ''
          exec python3 ${./workload.py} ${name} \
            --nginx ${patched}/sbin/nginx --baseline ${baseline}/sbin/nginx "$@"
        '';
      };
      app = name: { type = "app"; program = "${experiment name}/bin/${name}"; };
      in { inherit pkgs patched baseline harness app; };
    in {
      packages = each (system: p: { default = p.patched; inherit (p) baseline; });
      apps = each (system: p: { demo = p.app "demo"; bench = p.app "bench"; });
      devShells = each (system: p: {
        default = p.pkgs.mkShell {
          packages = with p.pkgs; [ python3 ffmpeg-headless wrk perl pcre2 zlib ];
          TEST_NGINX_BINARY = "${p.patched}/sbin/nginx";
          TEST_NGINX_BASELINE = "${p.baseline}/sbin/nginx";
          NGINX_TESTS = p.harness;
          PERL5LIB = "${p.harness}/lib";
        };
        # Compiler and libraries only, for building the working tree.
        build = p.pkgs.mkShell {
          packages = with p.pkgs; [ perl pcre2 zlib ];
          NGINX_TESTS = p.harness;
          PERL5LIB = "${p.harness}/lib";
        };
      });
    };
}
