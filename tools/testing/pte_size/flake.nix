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

      # Compile all static (musl) test programs. The dynamic helper
      # is built separately below with glibc so ld-linux.so code paths
      # are actually exercised.
      testSources = builtins.filter
        (f:
          builtins.match "test_.*\\.c" f != null
          && f != "test_dynamic_helper.c")
        (builtins.attrNames (builtins.readDir ./tests));

      tests = map
        (f:
          let name = builtins.replaceStrings [ ".c" ] [ "" ] f;
          in mkTest name ./tests/${f})
        testSources;

      # Dynamically linked helper, glibc. Needs ld-linux.so + libc.so
      # staged into the initrd alongside it (see initrd rule below).
      dynamicHelper = pkgs.stdenv.mkDerivation {
        name = "test_dynamic_helper";
        src = ./tests/test_dynamic_helper.c;
        dontUnpack = true;
        buildPhase = ''
          $CC -O2 -Wall -o test_dynamic_helper $src -I ${./tests}
        '';
        installPhase = ''
          mkdir -p $out/bin
          cp test_dynamic_helper $out/bin/
        '';
      };

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
        nativeBuildInputs = [ pkgs.cpio pkgs.patchelf ];
      } ''
        root=$(mktemp -d)

        # Create directory structure
        mkdir -p $root/{proc,sys,dev,tmp,tests,lib,lib64}

        # Install init as /init
        cp ${init}/bin/init $root/init
        chmod 755 $root/init

        # Install all static test binaries
        ${builtins.concatStringsSep "\n" (map (t: ''
          for f in ${t}/bin/*; do
            cp "$f" $root/tests/
            chmod 755 "$root/tests/$(basename $f)"
          done
        '') tests)}

        # Install the dynamic helper and rewrite its ELF interpreter +
        # rpath to look in /lib, then stage libc + the loader there.
        cp ${dynamicHelper}/bin/test_dynamic_helper $root/tests/
        chmod 755 $root/tests/test_dynamic_helper

        interp=$(patchelf --print-interpreter $root/tests/test_dynamic_helper)
        patchelf --set-interpreter /lib/ld-linux-x86-64.so.2 \
                 --set-rpath /lib \
                 $root/tests/test_dynamic_helper

        # Copy the loader to /lib and every needed .so from the same
        # glibc store path alongside it.
        cp -L "$interp" $root/lib/ld-linux-x86-64.so.2
        chmod 755 $root/lib/ld-linux-x86-64.so.2
        libdir=$(dirname "$interp")
        for so in libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 \
                  librt.so.1 libresolv.so.2; do
          if [ -e "$libdir/$so" ]; then
            cp -L "$libdir/$so" "$root/lib/$so"
            chmod 755 "$root/lib/$so"
          fi
        done

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
