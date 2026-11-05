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
          cmake
          meson
          ninja
          pkg-config
          wayland-scanner
        ];

        buildInputs = with pkgs; [
          boost
          fcitx5
          fontconfig
          freetype
          libime
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
          cmake
          meson
          ninja
          pkg-config
          wayland-scanner
        ];

        buildInputs = with pkgs; [
          boost
          fcitx5
          fontconfig
          freetype
          libime
          libxkbcommon
          libxcb
          xcbutil
          xcbutilkeysyms
          xcb-imdkit
          wayland
        ];

        doCheck = true;

        configurePhase = ''
          runHook preConfigure
          meson setup build --prefix=$out --buildtype=release
          runHook postConfigure
        '';

        buildPhase = ''
          runHook preBuild
          meson compile -C build
          runHook postBuild
        '';

        checkPhase = ''
          runHook preCheck
          meson test -C build --no-rebuild --print-errorlogs
          runHook postCheck
        '';

        installPhase = ''
          runHook preInstall
          meson install -C build
          runHook postInstall
        '';
      };
    };
}
