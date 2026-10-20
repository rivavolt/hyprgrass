#!/bin/sh

set -eux

echoerr () {
	echo $@ >&2
}

if [ -z "${1:-}" ]; then
	echoerr "Usage:"
	echoerr ""
	echoerr "	test-pin HYPRLAND_TAG [HYPRGRASS_REV]"
	echoerr ""
	echoerr "Builds and tests HYPRGRASS_REV against the Hyprland release HYPRLAND_TAG as packaged"
	echoerr "in nixos-unstable, and prints the hyprpm.toml pin of both commits."
	echoerr ""
	echoerr "HYPRGRASS_REV defaults to 'main'"
	exit 1
fi

hyprlandTag=${1}
hyprgrassRev=${2:-main}

hyprlandCommit="$(git ls-remote https://github.com/hyprwm/Hyprland.git "${hyprlandTag}" | cut -f 1)"
hyprgrassCommit="$(git rev-parse "${hyprgrassRev}")"

# Hyprland comes from nixpkgs, the distro build hyprpm users compile plugins against, which nixpkgs keeps buildable and caches; a release's own flake.lock can lock a nixpkgs it does not build with (v0.56.2's has glaze 8, which its CMakeLists rejects).
nix build --no-link --impure --expr "
	let
		hyprgrass = builtins.getFlake \"git+file://$(git rev-parse --show-toplevel)?rev=${hyprgrassCommit}\";
		pkgs = import (builtins.getFlake \"${NIXPKGS:-github:NixOS/nixpkgs/nixos-unstable}\") {
			system = builtins.currentSystem;
			overlays = [hyprgrass.overlays.default];
		};
	in
		assert pkgs.lib.assertMsg (\"v\${pkgs.hyprland.version}\" == \"${hyprlandTag}\")
			\"nixpkgs packages Hyprland v\${pkgs.hyprland.version}, not ${hyprlandTag}\";
		pkgs.hyprlandPlugins.hyprgrass.override {runTests = true;}
" >&2

echo "[\"${hyprlandCommit}\", \"${hyprgrassCommit}\"], # ${hyprlandTag}"
