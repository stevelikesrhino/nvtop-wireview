# WireView power rails

This fork can display the six connector-pin readings from a Thermal Grizzly WireView Pro II directly below a GPU.
The provider is compiled into `nvtop`; it does not build or launch a helper, daemon, or kernel module.

## Build and install

These commands target Debian or Ubuntu, including WSL2, with an NVIDIA GPU.

```bash
sudo apt update
sudo apt install -y build-essential cmake git libncurses-dev libdrm-dev libsystemd-dev
git clone --branch feature/wireview-power-rails https://github.com/stevelikesrhino/nvtop.git
cd nvtop
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNVIDIA_SUPPORT=ON -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j"$(nproc)"
cmake --install build
```

Run `~/.local/bin/nvtop`. If you want to run `nvtop` by name, make sure `$HOME/.local/bin` is before `/usr/bin` in
your `PATH`.

## Connect WireView

Run the setup script as your normal user. It loads the serial driver, installs a WireView-specific udev rule, and adds
your user to `dialout`:

```bash
./scripts/setup-wireview-linux.sh
```

On native Linux, unplug and reconnect WireView, then sign out and back in.

On WSL2, the script also enables systemd and prints the remaining Windows commands. Run `bind` once as administrator,
then start WSL and attach the device:

```powershell
usbipd list
usbipd bind --busid <BUSID>
wsl --shutdown
wsl -d Debian -- true
usbipd attach --wsl Debian --busid <BUSID>
```

For automatic reattachment, use this instead of the last `attach` command. Keep it running or start it with a Windows
logon task:

```powershell
usbipd attach --wsl Debian --busid <BUSID> --auto-attach --unplugged
```

Confirm that Linux sees the device, then run nvtop:

```bash
ls -l /dev/ttyACM*
~/.local/bin/nvtop
```

Do not run nvtop with `sudo` or make the serial device world-writable. The udev rule grants access only to `dialout`.

## Linux access

The provider discovers USB serial devices with vendor/product ID `0483:5740` under `/sys/class/tty/ttyACM*` and opens
the matching `/dev/ttyACM*` at 115200 baud. The user running nvtop needs read/write access to that TTY.

Only identification (`0x01`), UID (`0x02`), and sensor (`0x04`) commands are implemented. The provider never sends device
configuration, display, calibration, logging, reset, or firmware-update commands.

The device node is opened without following symlinks and must be the expected TTY character device. nvtop takes advisory
and TTY exclusive locks, restores serial state it changed, verifies the exact welcome/vendor response, rejects malformed
or physically implausible sensor frames, bounds all reads and writes, and throttles rediscovery after failures. If more
than one serial device uses the same USB ID, nvtop checks each candidate until it authenticates a WireView.

## Display and mapping

With exactly one GPU, a detected WireView is mapped to it and enabled automatically. F2 → Header → “Display WireView
connector power rails” toggles a six-row per-pin view. Each row shows a selectable current, power, or voltage value and
a current bar. The bars use a fixed 10 A scale while all pins are at or below 10 A. If any pin exceeds 10 A, the bars
scale to the busiest pin and every pin above 10 A is highlighted red. Change “WireView value beside each current bar”
to select the value. Each row also shows that pin’s highest current since nvtop started. F12 persists both settings.

For a multi-GPU machine, map the monitor explicitly in the generated config:

```ini
[HeaderOption]
PowerRails = true
PowerRailDisplayValue = Current
PowerRailsPdev = 0000:01:00.0
```

The serial transport and protocol decoder implement the generic `power_rail_snapshot` interface in
`include/nvtop/power_rails.h`. A future hwmon or different meter provider can populate the same model without changing
the GPU vendor backends or table renderer.
