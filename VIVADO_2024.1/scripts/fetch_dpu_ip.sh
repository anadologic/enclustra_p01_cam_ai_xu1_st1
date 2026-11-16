#!/usr/bin/env bash
# ----------------------------------------------------------------------------------------------------
# Downloads the AMD DPUCZDX8G v4.1 IP into ip_repo/ (not tracked in git: AMD proprietary IP).
#
# Source: Vitis AI 3.0 DPU TRD (DPUCZDX8G_VAI_v3.0.tar.gz). The Vitis AI v4.0 release notes list
# this DPU IP as verified with Vivado / PetaLinux 2024.1 (no DPUCZDX8G changes since 3.0).
# ----------------------------------------------------------------------------------------------------
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
url='https://www.xilinx.com/bin/public/openDownload?filename=DPUCZDX8G_VAI_v3.0.tar.gz'
md5='4226a7cfcb2c280d699f51201e912f32'
ip='DPUCZDX8G_v4_1_0'
dst="$root/ip_repo/$ip"

if [[ -f "$dst/component.xml" ]]; then
  echo "DPU IP already present: $dst"
  exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Downloading DPUCZDX8G_VAI_v3.0.tar.gz ..."
curl -fL --retry 3 -o "$tmp/trd.tar.gz" "$url"
echo "$md5  $tmp/trd.tar.gz" | md5sum -c -

tar -xzf "$tmp/trd.tar.gz" -C "$tmp" "DPUCZDX8G_VAI_v3.0/dpu_ip/$ip"
mkdir -p "$root/ip_repo"
mv "$tmp/DPUCZDX8G_VAI_v3.0/dpu_ip/$ip" "$dst"
echo "DPU IP installed: $dst"
