#!/bin/bash
# install_precice.sh [--no-python] [--no-adapters] [--no-tutorials] [--force]
#   installs the latest preCICE release and what Q-Continuum's coupled runs use, each where Q-Continuum looks for it:
#   - libprecice3 (the .deb from github.com/precice/precice for this Ubuntu, checksum verified) and its apt dependencies
#   - pyprecice, the Python bindings, for python3 (pip --user)
#   - the OpenFOAM adapter, built into $FOAM_USER_LIBBIN (needs OpenFOAM; skipped without it)
#   - ccx_preCICE, the CalculiX adapter (.deb from github.com/precice/calculix-adapter)
#   - the preCICE tutorials in ~/src/precice-tutorials
# Anything already installed at the latest version is left alone; --force reinstalls it.
set -euo pipefail
PY=1; ADAPTERS=1; TUTORIALS=1; FORCE=0
for a in "$@"; do
    case "$a" in
        --no-python) PY=0 ;; --no-adapters) ADAPTERS=0 ;; --no-tutorials) TUTORIALS=0 ;; --force) FORCE=1 ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "Unknown option: $a (try --help)"; exit 1 ;;
    esac
done
SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO=sudo
. /etc/os-release
CODENAME=${VERSION_CODENAME:-noble}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fails=""

# release_deb owner/repo: download the newest release's .deb for this Ubuntu into $TMP and check its .sha256; prints its path
release_deb() {
    local url
    url=$(curl -fsSL "https://api.github.com/repos/$1/releases/latest" | python3 -I -c '
import json, sys
for a in json.load(sys.stdin).get("assets", []):
    if a["name"].endswith("_" + sys.argv[1] + ".deb"): print(a["browser_download_url"]); break' "$CODENAME") || true
    [ -n "$url" ] || { echo "No $1 package for Ubuntu $CODENAME on GitHub." >&2; return 1; }
    local f=$TMP/$(basename "$url")
    curl -fsSL -o "$f" "$url" && curl -fsSL -o "$f.sha256" "$url.sha256" || { echo "Could not download $url" >&2; return 1; }
    (cd "$TMP" && sha256sum -c --quiet "$(basename "$f").sha256") >&2 || { echo "Checksum mismatch for $(basename "$f")" >&2; return 1; }
    echo "$f"
}

# install_deb file: apt-get install it (with its dependencies) unless that version is installed already
install_deb() {
    local pkg ver
    pkg=$(dpkg-deb -f "$1" Package); ver=$(dpkg-deb -f "$1" Version)
    if [ "$FORCE" = 0 ] && [ "$(dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null || true)" = "$ver" ]; then
        echo "$pkg $ver is installed already."
        return 0
    fi
    echo "Installing $pkg $ver and its dependencies..."
    $SUDO apt-get install -y --reinstall "$1"
}

echo "== preCICE library =="
$SUDO apt-get update -qq
deb=$(release_deb precice/precice) && install_deb "$deb" || fails="$fails libprecice"
command -v precice-version > /dev/null && echo "preCICE $(precice-version | head -1)"

if [ "$PY" = 1 ]; then
    echo "== pyprecice (Python bindings) =="
    $SUDO apt-get install -y -qq python3-pip python3-dev python3-numpy python3-mpi4py pkg-config > /dev/null
    if [ "$FORCE" = 0 ] && python3 -c "import precice" 2> /dev/null; then
        echo "pyprecice is installed already ($(python3 -c 'import precice; print(precice.__version__)' 2>/dev/null || echo '?'))."
    else
        # matched to the library's major.minor; Ubuntu 24.04 needs --break-system-packages for a --user install
        mm=$(pkg-config --modversion libprecice 2>/dev/null | cut -d. -f1,2)
        brk=""; python3 -m pip install --help 2>/dev/null | grep -q -- --break-system-packages && brk=--break-system-packages
        python3 -m pip install --user $brk --no-cache-dir "pyprecice${mm:+==$mm.*}" \
            || python3 -m pip install --user $brk --no-cache-dir pyprecice || fails="$fails pyprecice"
    fi
fi

if [ "$ADAPTERS" = 1 ]; then
    echo "== OpenFOAM adapter =="
    ofrc=""
    for d in ${WM_PROJECT_DIR:+"$WM_PROJECT_DIR"} /usr/lib/openfoam/openfoam* /opt/openfoam* /opt/OpenFOAM* "$HOME"/OpenFOAM/OpenFOAM-*; do
        [ -f "$d/etc/bashrc" ] && { ofrc=$d/etc/bashrc; break; }
    done
    if [ -z "$ofrc" ]; then
        echo "OpenFOAM isn't installed: skipping its adapter (install OpenFOAM, then run this again)."
    else
        # OpenFOAM's bashrc trips over set -u and returns non-zero for harmless reasons: source it in a plain bash
        libbin=$(bash -c "source '$ofrc' > /dev/null 2>&1; echo \$FOAM_USER_LIBBIN")
        ofver=$(bash -c "source '$ofrc' > /dev/null 2>&1; echo \$WM_PROJECT_VERSION")
        if [ "$FORCE" = 0 ] && [ -f "$libbin/libpreciceAdapterFunctionObject.so" ]; then
            echo "Built already: $libbin/libpreciceAdapterFunctionObject.so"
        else
            src=$HOME/src/openfoam-adapter
            case "$ofver" in
                v*) branch=master ;;                                  # openfoam.com (v2606 ...)
                *)  branch=OpenFOAM${ofver%%.*} ;;                    # openfoam.org 10, 9 ... have their own branches
            esac
            if [ "$branch" != master ] && ! git ls-remote --exit-code --heads https://github.com/precice/openfoam-adapter "$branch" > /dev/null; then
                echo "The adapter has no branch for OpenFOAM $ofver; trying master."; branch=master
            fi
            if [ "$branch" = master ]; then
                # the newest release, not master's tip
                tag=$(curl -fsSL https://api.github.com/repos/precice/openfoam-adapter/releases/latest | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["tag_name"])' 2>/dev/null || echo master)
                branch=$tag
            fi
            rm -rf "$src"; mkdir -p "$HOME/src"
            echo "Building the adapter ($branch) for OpenFOAM $ofver..."
            if git -c advice.detachedHead=false clone -q --depth 1 --branch "$branch" https://github.com/precice/openfoam-adapter.git "$src" \
               && bash -c "source '$ofrc' > /dev/null 2>&1; cd '$src' && ./Allwmake" > "$src/build.log" 2>&1 \
               && [ -f "$libbin/libpreciceAdapterFunctionObject.so" ]; then
                echo "Built $libbin/libpreciceAdapterFunctionObject.so"
            else
                tail -20 "$src/build.log" 2> /dev/null || true
                echo "The OpenFOAM adapter didn't build (full log: $src/build.log)."; fails="$fails openfoam-adapter"
            fi
        fi
    fi

    echo "== CalculiX adapter (ccx_preCICE) =="
    deb=$(release_deb precice/calculix-adapter) && install_deb "$deb" || fails="$fails calculix-adapter"
fi

if [ "$TUTORIALS" = 1 ]; then
    echo "== preCICE tutorials =="
    tut=$HOME/src/precice-tutorials
    if [ -d "$tut/.git" ]; then
        echo "Updating $tut..."
        git -C "$tut" pull -q --ff-only || echo "Could not update $tut (local changes?): leaving it as it is."
    else
        # master follows the latest preCICE release (develop is the next one)
        mkdir -p "$HOME/src"
        git clone -q --depth 1 --branch master https://github.com/precice/tutorials.git "$tut" && echo "Cloned into $tut" \
            || fails="$fails tutorials"
    fi
fi

echo "== Check =="
command -v precice-version > /dev/null && echo "  libprecice   $(pkg-config --modversion libprecice 2>/dev/null)" || echo "  libprecice   MISSING"
[ "$PY" = 1 ] && { python3 -c "import precice" 2> /dev/null && echo "  pyprecice    $(python3 -c 'import precice; print(precice.__version__)' 2>/dev/null)" || echo "  pyprecice    MISSING"; }
if [ "$ADAPTERS" = 1 ]; then
    [ -n "${libbin:-}" ] && { [ -f "$libbin/libpreciceAdapterFunctionObject.so" ] && echo "  OpenFOAM     adapter in $libbin" || echo "  OpenFOAM     adapter MISSING"; }
    command -v ccx_preCICE > /dev/null && echo "  CalculiX     $(command -v ccx_preCICE)" || echo "  CalculiX     ccx_preCICE MISSING"
fi
[ "$TUTORIALS" = 1 ] && { [ -d "$HOME/src/precice-tutorials" ] && echo "  tutorials    $HOME/src/precice-tutorials" || echo "  tutorials    MISSING"; }
if [ -n "$fails" ]; then echo "Not installed:$fails"; exit 1; fi
echo "preCICE is ready. Restart Q-Continuum; the Coupling page uses it."
