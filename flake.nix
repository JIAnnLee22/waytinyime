{
  description = "waytinyime - native Wayland input method";

  inputs = {
    nixpkgs.url = "nixpkgs";
  };

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        nativeBuildInputs = with pkgs; [
          meson
          ninja
          pkg-config
          wayland-scanner
        ];

        buildInputs = with pkgs; [
          fontconfig
          freetype
          libxkbcommon
          libxcb
          xcbutil
          xcbutilkeysyms
          xcb-imdkit
          wayland
        ];
      };

      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "waytinyime";
        version = "0.1.0";

        src = pkgs.lib.cleanSourceWith {
          src = ./.;
          filter =
            path: type:
            let
              name = baseNameOf path;
            in
            pkgs.lib.cleanSourceFilter path type && name != "build" && name != "result";
        };

        nativeBuildInputs = with pkgs; [
          meson
          ninja
          pkg-config
          wayland-scanner
        ];

        buildInputs = with pkgs; [
          fontconfig
          freetype
          libxkbcommon
          libxcb
          xcbutil
          xcbutilkeysyms
          xcb-imdkit
          wayland
        ];

        mesonBuildType = "release";
      };
    };
}
