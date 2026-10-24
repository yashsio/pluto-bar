{
  description = "pluto-bar - a dwm-style status bar for Hyprland and river";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          nativeBuildInputs = with pkgs; [
            gcc
            gnumake
            pkg-config
            wayland-scanner
          ];

          buildInputs = with pkgs; [
            wayland
            wlr-protocols
            wayland-protocols
            cairo
            pango
            fontconfig
          ];

          shellHook = ''
            export WLR_PROTOCOLS="${pkgs.wlr-protocols}/share/wlr-protocols"
            export WAYLAND_PROTOCOLS="${pkgs.wayland-protocols}/share/wayland-protocols"
            echo "pluto-bar dev shell"
            echo "  make          build"
            echo "  ./pluto-bar   run (needs a Hyprland or river session)"
          '';
        };
      });

      packages = forAllSystems (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "pluto-bar";
          version = "0.1";

          src = self;

          nativeBuildInputs = with pkgs; [
            gnumake
            pkg-config
            wayland-scanner
          ];

          buildInputs = with pkgs; [
            wayland
            wlr-protocols
            wayland-protocols
            cairo
            pango
            fontconfig
          ];

          makeFlags = [
            "WLR_PROTOCOLS=${pkgs.wlr-protocols}/share/wlr-protocols"
            "WAYLAND_PROTOCOLS=${pkgs.wayland-protocols}/share/wayland-protocols"
          ];

          preBuild = ''
            make clean
          '';

          installPhase = ''
            runHook preInstall
            install -Dm755 pluto-bar $out/bin/pluto-bar
            install -Dm755 status.sh $out/bin/pluto-status
            runHook postInstall
          '';

          meta = with pkgs.lib; {
            description = "A suckless, dwm-style status bar for Hyprland and river";
            license = licenses.mit;
            platforms = platforms.linux;
            mainProgram = "pluto-bar";
          };
        };
      });
    };
}
