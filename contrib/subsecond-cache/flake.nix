{
  description = "nginx millisecond cache reference experiment";
  inputs.nixpkgs.url = "https://channels.nixos.org/nixpkgs-unstable/nixexprs.tar.zst";

  outputs = { self, nixpkgs }:
    let
      system = "aarch64-darwin";
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
    in {
      packages.${system} = { default = patched; inherit baseline; };
      apps.${system} = { demo = app "demo"; bench = app "bench"; };
      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [ python3 ffmpeg-headless wrk perl pcre2 zlib ];
        TEST_NGINX_BINARY = "${patched}/sbin/nginx";
        TEST_NGINX_BASELINE = "${baseline}/sbin/nginx";
        NGINX_TESTS = harness;
        PERL5LIB = "${harness}/lib";
      };
    };
}
