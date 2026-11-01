#!/bin/bash
# install_chrono_deme.sh [--chrono] [--deme] [--jobs N] [--force]
#   builds Project Chrono (latest release) and DEM-Engine (DEME) from source where Q-Continuum finds them:
#   ~/src/chrono/build and ~/src/DEM-Engine/build, with the apt packages, Blaze, Spectra and (for an NVIDIA GPU)
#   the CUDA toolkit they need. With neither --chrono nor --deme it does both. A build Q-Continuum already finds
#   (also ~/src/chrono-*/build, ~/opt/chrono-*, ~/opt/deme-*) is left alone unless --force.
#
#   DEME and Chrono's DEM and FSI-SPH modules need CUDA: without an NVIDIA GPU, DEME is skipped and Chrono is built
#   without them. The build takes 20-60 minutes; it uses at most one job per 2 GB of memory (a full-width build runs
#   out of memory on 32 GB). QC_CUDA_VERSION picks the toolkit (default 12-8, the first with RTX 50-series support).
set -euo pipefail
DO_CHRONO=0; DO_DEME=0; FORCE=0; JOBS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --chrono) DO_CHRONO=1 ;; --deme) DO_DEME=1 ;; --force) FORCE=1 ;;
        --jobs) JOBS=$2; shift ;; --jobs=*) JOBS=${1#--jobs=} ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1 (try --help)"; exit 1 ;;
    esac
    shift
done
[ "$DO_CHRONO" = 0 ] && [ "$DO_DEME" = 0 ] && { DO_CHRONO=1; DO_DEME=1; }
SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO=sudo
SRC=$HOME/src; DEPS=$SRC/deps
CUDA_VER=${QC_CUDA_VERSION:-12-8}
IS_WSL=0; grep -qi microsoft /proc/version 2>/dev/null && IS_WSL=1

if [ -z "$JOBS" ]; then
    memgb=$(awk '/MemTotal/ {print int($2 / 1048576)}' /proc/meminfo)
    JOBS=$(( memgb / 2 )); [ "$JOBS" -gt "$(nproc)" ] && JOBS=$(nproc); [ "$JOBS" -gt 16 ] && JOBS=16; [ "$JOBS" -lt 1 ] && JOBS=1
fi

# what Q-Continuum finds (src/program.c chrono_dir / deme_dir)
found() { for p in "$@"; do for f in $p; do [ -f "$f" ] && { echo "$f"; return 0; }; done; done; return 1; }
chrono_found() { found "$SRC/chrono/build/cmake/ChronoConfig.cmake" "$HOME/chrono/build/cmake/ChronoConfig.cmake" \
                       "$SRC/chrono-*/build/cmake/ChronoConfig.cmake" "$HOME/opt/chrono-*/lib/cmake/Chrono/ChronoConfig.cmake" \
                       /usr/local/lib/cmake/Chrono/ChronoConfig.cmake; }
deme_found() { found "$SRC/DEM-Engine/build/DEME/DEMEConfig.cmake" "$HOME/DEM-Engine/build/DEME/DEMEConfig.cmake" \
                     "$SRC/*/build/DEME/DEMEConfig.cmake" "$HOME/opt/deme-*/lib/cmake/DEME/DEMEConfig.cmake" \
                     /usr/local/lib/cmake/DEME/DEMEConfig.cmake; }
if [ "$FORCE" = 0 ]; then
    if [ "$DO_CHRONO" = 1 ] && f=$(chrono_found); then echo "Chrono is built already: $f (--force rebuilds it)"; DO_CHRONO=0; fi
    if [ "$DO_DEME" = 1 ] && f=$(deme_found); then echo "DEME is built already: $f (--force rebuilds it)"; DO_DEME=0; fi
    [ "$DO_CHRONO" = 0 ] && [ "$DO_DEME" = 0 ] && exit 0
fi

# an NVIDIA GPU (on WSL the driver is Windows'; nvidia-smi is in /usr/lib/wsl/lib)
GPU=""; ARCH=""
smi=$(command -v nvidia-smi || ls /usr/lib/wsl/lib/nvidia-smi 2>/dev/null || true)
if [ -n "$smi" ] && q=$("$smi" --query-gpu=name,compute_cap --format=csv,noheader 2>/dev/null | head -1) && [ -n "$q" ]; then
    GPU=${q%%,*}; ARCH=$(echo "${q##*,}" | tr -d ' .')
    echo "GPU: $GPU (CUDA architecture $ARCH)"
else
    echo "No NVIDIA GPU found: DEME is skipped, and Chrono is built without its DEM and FSI-SPH modules."
    [ "$DO_DEME" = 1 ] && [ "$DO_CHRONO" = 0 ] && exit 1
    DO_DEME=0
fi

echo "== Packages =="
export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update -qq
$SUDO apt-get install -y -qq build-essential cmake ninja-build git wget curl pkg-config \
    libeigen3-dev libirrlicht-dev libomp-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libgl1-mesa-dev libglu1-mesa-dev mesa-common-dev python3-dev python3-venv swig > /dev/null
NVCC=""
if [ -n "$GPU" ]; then
    NVCC=$(ls -d /usr/local/cuda-*/bin/nvcc 2>/dev/null | sort -V | tail -1 || true)
    if [ -z "$NVCC" ]; then
        # NVIDIA's repo: the WSL one has no Linux driver in it (WSL uses the Windows driver), so it can't replace that
        repo=ubuntu2404; [ "$IS_WSL" = 1 ] && repo=wsl-ubuntu
        if ! dpkg -s cuda-keyring > /dev/null 2>&1; then
            wget -q -O /tmp/cuda-keyring.deb "https://developer.download.nvidia.com/compute/cuda/repos/$repo/x86_64/cuda-keyring_1.1-1_all.deb"
            $SUDO dpkg -i /tmp/cuda-keyring.deb; rm -f /tmp/cuda-keyring.deb
            $SUDO apt-get update -qq
        fi
        echo "Installing the CUDA toolkit $CUDA_VER (a few GB)..."
        $SUDO apt-get install -y -qq "cuda-toolkit-$CUDA_VER" > /dev/null
        NVCC=/usr/local/cuda-${CUDA_VER/-/.}/bin/nvcc
    fi
    [ -x "$NVCC" ] || { echo "No nvcc after installing CUDA ($NVCC)."; exit 1; }
    echo "CUDA: $NVCC"
    export PATH=$(dirname "$NVCC"):$PATH
fi
mkdir -p "$SRC" "$DEPS"

if [ "$DO_DEME" = 1 ]; then
    echo "== DEM-Engine =="
    d=$SRC/DEM-Engine
    if [ -d "$d/.git" ]; then git -C "$d" pull -q --ff-only || true; git -C "$d" submodule update -q --init --recursive
    else git clone -q --recursive https://github.com/projectchrono/DEM-Engine.git "$d"; fi
    cmake -S "$d" -B "$d/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_COMPILER="$NVCC" \
        -DCMAKE_CUDA_ARCHITECTURES="$ARCH" -DDEME_BUILD_DEMOS=ON -DDEME_BUILD_PYTHON=OFF > "$d/build-configure.log" 2>&1 \
        || { tail -20 "$d/build-configure.log"; echo "Configuring DEME failed (log: $d/build-configure.log)"; exit 1; }
    echo "Building DEME with $JOBS jobs (10-20 minutes)..."
    cmake --build "$d/build" -j "$JOBS" > "$d/build.log" 2>&1 || { grep -m5 -E 'error' "$d/build.log"; echo "DEME failed to build (log: $d/build.log)"; exit 1; }
    [ -f "$d/build/DEME/DEMEConfig.cmake" ] && echo "DEME built: $d/build"
fi

if [ "$DO_CHRONO" = 1 ]; then
    echo "== Chrono =="
    # Blaze (Chrono::Multicore) and Spectra's develop branch (Chrono::Modal needs its Krylov-Schur solver)
    if [ ! -f "$DEPS/blaze-3.8/blaze/Blaze.h" ]; then
        curl -fsSL https://bitbucket.org/blaze-lib/blaze/downloads/blaze-3.8.tar.gz | tar xz -C "$DEPS"
    fi
    if [ ! -f "$DEPS/spectra/include/Spectra/KrylovSchurGEigsSolver.h" ]; then
        rm -rf "$DEPS/spectra"; git clone -q --depth 1 --branch develop https://github.com/yixuan/spectra.git "$DEPS/spectra"
    fi
    tag=$(curl -fsSL https://api.github.com/repos/projectchrono/chrono/releases/latest | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["tag_name"])')
    d=$SRC/chrono
    if [ -d "$d/.git" ]; then git -C "$d" fetch -q --depth 1 origin tag "$tag" && git -C "$d" -c advice.detachedHead=false checkout -q "$tag"
    else git -c advice.detachedHead=false clone -q --depth 1 --branch "$tag" https://github.com/projectchrono/chrono.git "$d"; fi
    echo "Chrono $tag"
    # 10.0.0: demo_YAML_vehicle uses ChVehicleVisualSystem without including it when VSG is off
    y=$d/src/demos/yaml/demo_YAML_vehicle.cpp
    if [ -f "$y" ] && ! grep -q 'chrono_vehicle/ChVehicleVisualSystem.h' "$y"; then
        sed -i 's|#include "chrono_vehicle/driver/ChInteractiveDriver.h"|&\n#include "chrono_vehicle/ChVehicleVisualSystem.h"|' "$y"
    fi
    gpu_on=OFF; cuda=()
    [ -n "$GPU" ] && { gpu_on=ON; cuda=(-DCMAKE_CUDA_COMPILER="$NVCC" -DCMAKE_CUDA_ARCHITECTURES="$ARCH"); }
    cmake -S "$d" -B "$d/build" -G Ninja -DCMAKE_BUILD_TYPE=Release "${cuda[@]}" \
        -DCH_ENABLE_MODULE_IRRLICHT=ON -DCH_ENABLE_MODULE_POSTPROCESS=ON -DCH_ENABLE_MODULE_PARSERS=ON \
        -DCH_ENABLE_MODULE_VEHICLE=ON -DCH_ENABLE_MODULE_VEHICLE_MODELS=ON -DCH_ENABLE_MODULE_VEHICLE_COSIM=ON \
        -DCH_ENABLE_MODULE_MULTICORE=ON -DCH_ENABLE_MODULE_MODAL=ON \
        -DCH_ENABLE_MODULE_DEM=$gpu_on -DCH_ENABLE_MODULE_FSI=$gpu_on -DCH_ENABLE_MODULE_FSI_SPH=$gpu_on \
        -Dblaze_INCLUDE_DIR="$DEPS/blaze-3.8" -Dspectra_INCLUDE_DIR="$DEPS/spectra/include" \
        > "$d/build-configure.log" 2>&1 \
        || { tail -20 "$d/build-configure.log"; echo "Configuring Chrono failed (log: $d/build-configure.log)"; exit 1; }
    echo "Building Chrono with $JOBS jobs (20-40 minutes)..."
    cmake --build "$d/build" -j "$JOBS" > "$d/build.log" 2>&1 || { grep -m5 -E 'error' "$d/build.log"; echo "Chrono failed to build (log: $d/build.log)"; exit 1; }
    [ -f "$d/build/cmake/ChronoConfig.cmake" ] && echo "Chrono built: $d/build"
fi
echo "Done. Restart Q-Continuum; New case has the Chrono and DEME starters."
