#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

fail() {
  echo "Error: $*" >&2
  exit 1
}

if [[ "${1:-}" == "--help" ]]; then
  echo "Usage: $0"
  echo "Grant the current Linux user access to WireView Pro II."
  echo "On WSL, also configure systemd and print the required Windows commands."
  exit 0
fi

[[ $# -eq 0 ]] || fail "this script takes no arguments"
[[ "$(uname -s)" == "Linux" ]] || fail "this script requires Linux"
command -v sudo >/dev/null || fail "sudo is required"

target_user="${SUDO_USER:-${USER:-}}"
[[ -n "$target_user" && "$target_user" != "root" ]] || fail "run this script as your normal user, not root"
id "$target_user" >/dev/null 2>&1 || fail "user '$target_user' does not exist"
getent group dialout >/dev/null || fail "the dialout group does not exist"

is_wsl=false
if grep -qi microsoft /proc/sys/kernel/osrelease; then
  is_wsl=true
fi
if [[ "$is_wsl" == true ]]; then
  command -v awk >/dev/null || fail "awk is required"
fi

echo "Configuring WireView access for $target_user..."
sudo -v

temp_dir="$(mktemp -d)"
trap 'rm -rf -- "$temp_dir"' EXIT

printf '%s\n' \
  'SUBSYSTEM=="tty", KERNEL=="ttyACM*", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="5740", GROUP="dialout", MODE="0660"' \
  >"$temp_dir/70-wireview.rules"
sudo install -o root -g root -m 0644 "$temp_dir/70-wireview.rules" /etc/udev/rules.d/70-wireview.rules
sudo usermod -aG dialout "$target_user"
if [[ ! -d /sys/module/cdc_acm ]]; then
  command -v modprobe >/dev/null || fail "modprobe is required to load cdc_acm"
  sudo modprobe cdc_acm
fi

if command -v udevadm >/dev/null && command -v pgrep >/dev/null && pgrep -x systemd-udevd >/dev/null 2>&1; then
  sudo udevadm control --reload-rules
  sudo udevadm trigger --subsystem-match=tty
fi

if [[ "$is_wsl" == false ]]; then
  echo
  echo "Linux setup is complete. Unplug and reconnect WireView, then sign out and back in."
  echo "Run: ls -l /dev/ttyACM*"
  exit 0
fi

wsl_input="$temp_dir/wsl.conf.input"
wsl_output="$temp_dir/wsl.conf"
if sudo test -f /etc/wsl.conf; then
  sudo install -m 0644 /etc/wsl.conf "$wsl_input"
  if ! sudo test -e /etc/wsl.conf.wireview-backup; then
    sudo cp -p /etc/wsl.conf /etc/wsl.conf.wireview-backup
  fi
else
  : >"$wsl_input"
fi

awk '
  function section_name(line, name) {
    name = line
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", name)
    return tolower(name)
  }
  BEGIN { in_boot = 0; saw_boot = 0; wrote_systemd = 0 }
  /^[[:space:]]*\[[^]]+\][[:space:]]*$/ {
    if (in_boot && !wrote_systemd) {
      print "systemd=true"
      wrote_systemd = 1
    }
    in_boot = section_name($0) == "[boot]"
    if (in_boot)
      saw_boot = 1
    print
    next
  }
  in_boot && /^[[:space:]]*systemd[[:space:]]*=/ {
    if (!wrote_systemd) {
      print "systemd=true"
      wrote_systemd = 1
    }
    next
  }
  { print }
  END {
    if (in_boot && !wrote_systemd)
      print "systemd=true"
    if (!saw_boot) {
      print ""
      print "[boot]"
      print "systemd=true"
    }
  }
' "$wsl_input" >"$wsl_output"

printf '%s\n' 'cdc_acm' >"$temp_dir/wireview-modules.conf"
sudo install -o root -g root -m 0644 "$wsl_output" /etc/wsl.conf
sudo install -o root -g root -m 0644 "$temp_dir/wireview-modules.conf" /etc/modules-load.d/wireview.conf

distro="${WSL_DISTRO_NAME:-Debian}"
escaped_distro="${distro//\'/\'\'}"
quoted_distro="'$escaped_distro'"
busid=""
usbipd_exe="/mnt/c/Program Files/usbipd-win/usbipd.exe"
if [[ -x "$usbipd_exe" ]]; then
  busid="$("$usbipd_exe" list 2>/dev/null | awk '$2 == "0483:5740" { print $1; exit }' || true)"
  if [[ ! "$busid" =~ ^[0-9]+-[0-9]+([.][0-9]+)*$ ]]; then
    busid=""
  fi
fi
busid="${busid:-<BUSID>}"

echo
echo "WSL setup is complete. /etc/wsl.conf was backed up once as /etc/wsl.conf.wireview-backup."
if [[ ! -x "$usbipd_exe" ]]; then
  echo "Install usbipd-win in Windows before continuing."
fi
echo "Run these commands in Windows PowerShell:"
echo
printf '  usbipd bind --busid %s                 # Administrator, once\n' "$busid"
printf '  wsl --shutdown\n'
printf '  wsl -d %s -- true\n' "$quoted_distro"
printf '  usbipd attach --wsl %s --busid %s\n' "$quoted_distro" "$busid"
echo
echo "For automatic reattachment, keep this running instead of the last attach command:"
printf '  usbipd attach --wsl %s --busid %s --auto-attach --unplugged\n' "$quoted_distro" "$busid"
