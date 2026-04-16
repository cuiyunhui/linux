{
  description = "Minimal initrd for PTE_SIZE page fault testing";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};

      # Use musl for fully static binaries
      musl = pkgs.pkgsStatic;

      # Compile a static C program
      mkTest = name: src:
        musl.stdenv.mkDerivation {
          inherit name src;
          dontUnpack = true;
          buildPhase = ''
            $CC -static -O2 -Wall -o ${name} $src -I ${./tests}
          '';
          installPhase = ''
            mkdir -p $out/bin
            cp ${name} $out/bin/
          '';
        };

      # Compile all test programs
      testSources = builtins.filter
        (f: builtins.match "test_.*\\.c" f != null)
        (builtins.attrNames (builtins.readDir ./tests));

      tests = map
        (f:
          let name = builtins.replaceStrings [ ".c" ] [ "" ] f;
          in mkTest name ./tests/${f})
        testSources;

      # Build the init binary
      init = musl.stdenv.mkDerivation {
        name = "pte-test-init";
        src = ./init.c;
        dontUnpack = true;
        buildPhase = ''
          $CC -static -O2 -Wall -o init $src
        '';
        installPhase = ''
          mkdir -p $out/bin
          cp init $out/bin/
        '';
      };

      # Pack everything into a cpio/gzip initrd
      initrd = pkgs.runCommand "pte-test-initrd" {
        nativeBuildInputs = [ pkgs.cpio ];
      } ''
        root=$(mktemp -d)

        # Create directory structure
        mkdir -p $root/{proc,sys,dev,tmp,tests}

        # Install init as /init
        cp ${init}/bin/init $root/init
        chmod 755 $root/init

        # Install all test binaries
        ${builtins.concatStringsSep "\n" (map (t: ''
          for f in ${t}/bin/*; do
            cp "$f" $root/tests/
            chmod 755 "$root/tests/$(basename $f)"
          done
        '') tests)}

        # Create the cpio archive
        (cd $root && find . | sort | cpio -o -H newc --quiet | gzip -9) > $out
      '';

    in {
      packages.${system} = {
        inherit initrd init;
        default = initrd;
      };

      # Convenience: nix run .#boot -- <bzImage-path>
      apps.${system}.boot = {
        type = "app";
        program = let
          script = pkgs.writeShellScript "boot-pte-test" ''
            KERNEL=''${1:-build-full/arch/x86/boot/bzImage}
            INITRD=${initrd}

            if [ ! -f "$KERNEL" ]; then
              echo "Usage: $0 <bzImage-path>"
              echo "Kernel not found: $KERNEL"
              exit 1
            fi

            echo "Booting: $KERNEL"
            echo "Initrd:  $INITRD"
            echo ""

            exec ${pkgs.qemu}/bin/qemu-system-x86_64 \
              -machine accel=kvm:tcg -cpu max \
              -m 512 \
              -smp 1 \
              -nographic \
              -no-reboot \
              -kernel "$KERNEL" \
              -initrd "$INITRD" \
              -append "console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 nokaslr nosmp norandmaps panic=-1 $QEMU_KERNEL_PARAMS"
          '';
        in "${script}";
      };
    };
}
