#!/usr/bin/env bash
# Q-Continuum installer for Ubuntu 24.04 and WSL (Ubuntu 24.04 under Windows 10/11).
#
#   ./install.sh                  build and install Q-Continuum to /usr/local
#   ./install.sh --with-openfoam  also install OpenFOAM v2606 from openfoam.com
#   ./install.sh --user           install to ~/.local instead (no sudo for the install step)
#   ./install.sh --uninstall      remove Q-Continuum (your cases and OpenFOAM are left alone)
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX=/usr/local
WITH_OF=ask
DEPS=1
UNINSTALL=0
JOBS="$(nproc 2>/dev/null || echo 2)"

for a in "$@"; do
  case "$a" in
    --with-openfoam) WITH_OF=yes ;;
    --no-openfoam)   WITH_OF=no ;;
    --user)          PREFIX="$HOME/.local" ;;
    --prefix=*)      PREFIX="${a#--prefix=}" ;;
    --no-deps)       DEPS=0 ;;
    --uninstall)     UNINSTALL=1 ;;
    -h|--help)       sed -n '2,7p' "$0"; exit 0 ;;
    *) echo "Unknown option: $a (try --help)"; exit 1 ;;
  esac
done

bold=$'\e[1m'; dim=$'\e[2m'; amber=$'\e[38;5;215m'; red=$'\e[31m'; green=$'\e[32m'; off=$'\e[0m'
step() { echo; echo "${amber}==>${off} ${bold}$*${off}"; }
note() { echo "    ${dim}$*${off}"; }
die()  { echo "${red}error:${off} $*" >&2; exit 1; }

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  command -v sudo >/dev/null || die "sudo is needed to install packages. Run as root or install sudo."
  SUDO=sudo
fi
# Only use sudo for the install step if the prefix isn't writable by us.
ISUDO=""
if [ "$PREFIX" != "$HOME/.local" ] && [ ! -w "$PREFIX" ]; then ISUDO="$SUDO"; fi

# A conda environment (even "base") puts its own compilers, cmake and libGL ahead of the system
# ones, which breaks the build. Build with conda's directories removed from PATH.
if [ -n "${CONDA_PREFIX:-}" ]; then
  PATH="$(echo "$PATH" | tr ':' '\n' | grep -v -e "conda" -e "mamba" | paste -sd: -)"
  unset CONDA_PREFIX PKG_CONFIG_PATH LD_LIBRARY_PATH CC CXX
  export PATH
fi

IS_WSL=0
if grep -qi microsoft /proc/version 2>/dev/null || [ -n "${WSL_DISTRO_NAME:-}" ]; then IS_WSL=1; fi

# ------------------------------------------------------------------ uninstall
if [ "$UNINSTALL" = 1 ]; then
  step "Removing Q-Continuum from $PREFIX"
  for f in bin/q-continuum share/applications/q-continuum.desktop share/icons/hicolor/scalable/apps/q-continuum.svg \
           share/icons/hicolor/256x256/apps/q-continuum.png share/pixmaps/q-continuum.png; do
    [ -e "$PREFIX/$f" ] && $ISUDO rm -f "$PREFIX/$f" && note "removed $PREFIX/$f"
  done
  note "Settings are kept in ~/.config/q-continuum (delete that folder to reset them)."
  exit 0
fi

echo "${bold}Q-Continuum installer${off}  ${dim}(OpenFOAM v2606 front end)${off}"
if [ -r /etc/os-release ]; then . /etc/os-release; note "System: ${PRETTY_NAME:-Linux}$([ $IS_WSL = 1 ] && echo ' on WSL')"; fi
if [ "${ID:-}" != "ubuntu" ] || [ "${VERSION_ID:-}" != "24.04" ]; then
  note "This script is written for Ubuntu 24.04. Other Debian-based systems will probably work."
fi

# ------------------------------------------------------------------ WSL display check
if [ $IS_WSL = 1 ]; then
  step "Checking WSL graphics (WSLg)"
  if [ -d /mnt/wslg ] && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; then
    note "WSLg is available: Q-Continuum will open as a normal Windows window."
  else
    echo "    ${red}WSLg was not detected.${off} Q-Continuum needs it to show its window."
    echo "    In Windows PowerShell run:   wsl --update   then   wsl --shutdown"
    echo "    and open Ubuntu again. (Windows 10 needs build 19044 or newer; Windows 11 has it built in.)"
    note "Continuing with the build anyway."
  fi
  case "$HERE" in
    /mnt/*) note "Tip: building from a Windows drive (/mnt/c/...) is slow. Copying the source into ~ is faster." ;;
  esac
fi

# ------------------------------------------------------------------ packages
if [ "$DEPS" = 1 ]; then
  step "Installing build tools and graphics libraries (apt)"
  export DEBIAN_FRONTEND=noninteractive
  $SUDO apt-get update -qq
  $SUDO apt-get install -y -qq \
    build-essential cmake git curl ca-certificates pkg-config \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev \
    libgl1-mesa-dev libgl1-mesa-dri libglu1-mesa-dev xdg-utils >/dev/null
  note "done"
fi

# ------------------------------------------------------------------ OpenFOAM
OF_ROOT=/usr/lib/openfoam/openfoam2606
OF_FOUND=""
for d in "$OF_ROOT" ${WM_PROJECT_DIR:+"$WM_PROJECT_DIR"} /usr/lib/openfoam/openfoam* /opt/openfoam* /opt/OpenFOAM* \
         "$HOME"/OpenFOAM/OpenFOAM-* "$HOME"/openfoam/OpenFOAM-* /usr/lib/openfoam*; do
  if [ -f "$d/etc/bashrc" ]; then OF_FOUND="$d"; break; fi
done
if [ -n "$OF_FOUND" ]; then
  step "OpenFOAM"
  note "Found $OF_FOUND"
  [ "$OF_FOUND" != "$OF_ROOT" ] && note "Q-Continuum lists every install it finds; pick one in Settings if it chose the wrong one."
else
  if [ "$WITH_OF" = ask ]; then
    if [ -t 0 ]; then
      echo
      read -r -p "OpenFOAM v2606 isn't installed. Install it now from openfoam.com (about 1 GB)? [Y/n] " ans
      case "${ans:-y}" in [Nn]*) WITH_OF=no ;; *) WITH_OF=yes ;; esac
    else
      WITH_OF=no
    fi
  fi
  if [ "$WITH_OF" = yes ]; then
    step "Installing OpenFOAM v2606 (openfoam.com Debian repository)"
    curl -fsSL https://dl.openfoam.com/add-debian-repo.sh | $SUDO bash
    $SUDO apt-get update -qq
    $SUDO apt-get install -y openfoam2606-default
    [ -f "$OF_ROOT/etc/bashrc" ] && note "Installed at $OF_ROOT" || note "Installed. Q-Continuum will look for it on start."
  else
    note "Skipping OpenFOAM. Install it later with:  ./install.sh --with-openfoam"
  fi
fi

# ------------------------------------------------------------------ build
step "Building Q-Continuum (downloads raylib 5.5 the first time; takes a minute or two)"
BUILD="$HERE/build"
# Fetch raylib 5.5 with git (pinned to the 5.5 release commit) unless a local copy was given.
if [ -z "${QC_RAYLIB_DIR:-}" ]; then
  QC_RAYLIB_DIR="$HERE/deps/raylib"
  if [ ! -f "$QC_RAYLIB_DIR/src/raylib.h" ]; then
    rm -rf "$QC_RAYLIB_DIR"; mkdir -p "$HERE/deps"
    git clone -q --depth 1 --branch 5.5 https://github.com/raysan5/raylib.git "$QC_RAYLIB_DIR" \
      || die "could not download raylib from github.com. Check that WSL has internet access (try: curl -I https://github.com)."
  fi
fi
rm -rf "$BUILD/_deps/raylib-subbuild"            # leftovers from an earlier failed download
EXTRA=("-DQC_RAYLIB_DIR=$QC_RAYLIB_DIR")
if ! cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" "${EXTRA[@]}" > "$HERE/cmake-configure.log" 2>&1; then
  tail -n 20 "$HERE/cmake-configure.log"
  die "configuring the build failed (full log: cmake-configure.log). If raylib could not be downloaded, check your internet connection, or set QC_RAYLIB_DIR to a raylib 5.5 source folder."
fi
cmake --build "$BUILD" -j "$JOBS" -- --no-print-directory 2>&1 | grep -E "error|Error" || true
[ -x "$BUILD/q-continuum" ] || die "the build failed. Run: cmake --build \"$BUILD\" to see the full output."
note "built $BUILD/q-continuum"

step "Installing to $PREFIX"
$ISUDO cmake --install "$BUILD" >/dev/null
note "$PREFIX/bin/q-continuum"
if command -v update-desktop-database >/dev/null; then $ISUDO update-desktop-database -q "$PREFIX/share/applications" 2>/dev/null || true; fi
if command -v gtk-update-icon-cache >/dev/null; then $ISUDO gtk-update-icon-cache -q -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true; fi

# This app used to be called Plenum: remove that earlier install so there is only one copy.
for P in "$PREFIX" "$HOME/.local"; do
  for f in bin/plenum share/applications/plenum.desktop share/icons/hicolor/scalable/apps/plenum.svg \
           share/icons/hicolor/256x256/apps/plenum.png share/pixmaps/plenum.png; do
    if [ -e "$P/$f" ]; then
      S=""; [ -w "$(dirname "$P/$f")" ] || S="$SUDO"
      $S rm -f "$P/$f" && note "removed the old Plenum file $P/$f"
    fi
  done
done

# Make sure ~/.local/bin is on PATH for --user installs.
if [ "$PREFIX" = "$HOME/.local" ] && ! echo ":$PATH:" | grep -q ":$HOME/.local/bin:"; then
  echo 'export PATH="$HOME/.local/bin:$PATH"' >> "$HOME/.bashrc"
  note "Added ~/.local/bin to PATH in ~/.bashrc (open a new terminal to pick it up)."
fi

# Create the OpenFOAM run folder Q-Continuum uses as its workspace.
RUN="$HOME/OpenFOAM/$(id -un)-v2606/run"
mkdir -p "$RUN"

# ------------------------------------------------------------------ ParaView hints
step "ParaView"
if [ $IS_WSL = 1 ]; then
  if ls "/mnt/c/Program Files"/ParaView*/bin/paraview.exe >/dev/null 2>&1; then
    note "Found Windows ParaView. Q-Continuum opens results in it directly."
  else
    note "Install ParaView for Windows from https://www.paraview.org/download/ (recommended on WSL:"
    note "it uses your GPU). Q-Continuum finds it in C:\\Program Files automatically."
  fi
else
  if command -v paraview >/dev/null; then note "Found $(command -v paraview)";
  else note "Not found. Install it with:  sudo apt install paraview   (or the paraview.org binaries)"; fi
fi

echo
echo "${green}Q-Continuum is installed.${off}"
echo "  Start it from a terminal:  ${bold}q-continuum${off}"
if [ $IS_WSL = 1 ]; then
  echo "  It also appears in the Windows Start menu under ${bold}Ubuntu${off}; this can take a minute or a WSL restart."
else
  echo "  or from your applications menu."
fi
echo "  Your cases live in ${bold}$RUN${off}"
