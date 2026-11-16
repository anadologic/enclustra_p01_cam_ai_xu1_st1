#!/usr/bin/env bash
# ----------------------------------------------------------------------------------------------------
# Downloads the AMD Vitis Vision library (L1 headers) into ext/Vitis_Libraries/ (not tracked in git).
#
# Source: https://github.com/Xilinx/Vitis_Libraries, tag v2024.1_update2 (Apache-2.0), pinned to its
# commit. Only vision/L1/include and the reference examples of the functions used by p01_preproc are
# checked out (sparse): L1 axiconv / cvtcolor / resize / demosaicing, L3 letterbox.
# ----------------------------------------------------------------------------------------------------
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
url='https://github.com/Xilinx/Vitis_Libraries.git'
tag='v2024.1_update2'
sha='3c8a3d59fe76157ffa0b31fb267961bea2e148f5'
dst="$root/ext/Vitis_Libraries"

if [[ -f "$dst/vision/L1/include/dnn/xf_letterbox.hpp" ]] &&
   [[ "$(git -C "$dst" rev-parse HEAD)" == "$sha" ]]; then
  echo "Vitis Vision already present: $dst ($tag)"
  exit 0
fi

rm -rf "$dst"
mkdir -p "$(dirname "$dst")"
git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" --filter=blob:none --sparse \
    "$url" "$dst"
git -C "$dst" sparse-checkout set --no-cone /LICENSE.txt /vision/L1/include/ \
    /vision/L1/examples/axiconv/ /vision/L1/examples/cvtcolor/ /vision/L1/examples/resize/ \
    /vision/L1/examples/demosaicing/ /vision/L3/examples/letterbox/
head="$(git -C "$dst" rev-parse HEAD)"
if [[ "$head" != "$sha" ]]; then
  echo "ERROR: $tag resolves to $head, expected $sha" >&2
  exit 1
fi
echo "Vitis Vision installed: $dst ($tag, $sha)"
