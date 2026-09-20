# Uniwill Control Center — Mechrevo fork

A Qt6/C++20 control center for Uniwill-based laptops, with a daemon, desktop GUI,
CLI, and optional GNOME or KDE Plasma integration. This fork of
[nanomatters/ucc](https://github.com/nanomatters/ucc) adds compatibility for the
**MECHREVO YAOSHI Series-X6AR55xY** and addresses desktop stalls, excessive polling,
Bluetooth retry loops, and unintended water-cooler commands.

The fork started from upstream commit
[`d2987af`](https://github.com/nanomatters/ucc/commit/d2987af6cbaa39a7357dc610d107da269d42b3a0).
The hardware work follows the
[Mechrevo Yaoshi Linux documentation](https://github.com/noteMASTER11/Mechrevo-Yaoshi-Linux/wiki).

## Key differences from upstream

| Area | Changes in this fork |
| --- | --- |
| Hardware identification | An exact four-field DMI match enables the known Mechrevo controller capabilities. Generic SKU `0001` is **not** globally whitelisted. The UI shows the native Mechrevo model and preserves the original SKU. |
| GNOME support | `BUILD_TRAY=OFF` removes the KDE/Plasma build requirement. The Qt6 GUI and GNOME panel extension work without installing Plasma. |
| Responsive monitoring | Periodic GUI D-Bus reads are asynchronous and bounded. Main telemetry updates every 2 seconds; hardware toggles update every 10 seconds. Monitoring pauses while the main window is hidden or minimized. |
| GNOME panel | Asynchronous D-Bus calls, shared in-flight reads, cancellation of stale results, and no periodic polling while the menu is closed. Brightness and cooler-fan sliders keep at most one command in flight and the latest pending value. |
| Window activation | A session D-Bus singleton prevents multiple GUI processes. The panel button activates the existing application, including a minimized window. The desktop launcher disables startup notification to avoid a lingering busy cursor. |
| Aquaris connection | Saved cooler identity and LED state can be imported from TCC. Selection prefers the saved address and name, with a unique-name fallback for a changed BLE address. Ambiguous matches are rejected. Bluetooth adapters appearing after daemon startup are handled. |
| Bluetooth retries | Failed connection attempts use delays of 5, 10, 20, 40, 80, then 120 seconds. Duplicate error signals do not restart the delay. Repeated fast reconnect failures fall back to discovery. Retry handling no longer resets the system Bluetooth adapter or removes the BlueZ device. |
| Cooler controls | Loading profiles or reflecting connection state no longer sends implicit pump-off or fan-zero commands. Failed status reads preserve the last known state instead of simulating a disconnect. |
| Error handling | GNOME preserves explicit `false` acknowledgements from the daemon. Profile method names and keyboard JSON serialization are corrected. Monitor history checks packet bounds and rejects stale replies. |
| Diagnostics | `ucc-gui --display N` waits for asynchronous samples and times out when the daemon is unavailable. An exact-model helper can audit fan readings or restore firmware fan control. |
| Packaging and drivers | Arch/CachyOS recipes include the tested suspend integration and a separate, pinned TUXEDO drivers 4.23.0 patch set. A TCC configuration converter is included without personal configurations. |

## Hardware scope

The Mechrevo compatibility alias requires **all** of these values:

| DMI field | Required value |
| --- | --- |
| `sys_vendor` | `MECHREVO` |
| `board_name` | `YAOSHI Series-X6AR55xY` |
| `board_version` | `Standard` |
| `product_sku` | `0001` |

The alias reuses the controller capabilities of `STELLARIS16I07`; it does not
change DMI data or identify the laptop as a TUXEDO model in the interface.
Validation used an Intel Core Ultra 9 275HX, RTX 5080 Laptop GPU, ITE RGB keyboard,
and Aquaris/CoolingSystem LCT22002 on CachyOS with GNOME/Wayland.

Hardware control remains experimental. Validation on this exact configuration
does not establish support for other Mechrevo models or firmware revisions.
Upstream hardware support is retained, but other models have not been retested
with this fork's service changes. The firmware-fan helper is restricted to the
exact Mechrevo DMI match above.

## Components

- **uccd** — hardware control, profiles, monitoring, and Bluetooth cooler service.
- **libucc-dbus** — shared D-Bus client library for the Qt applications.
- **ucc-gui** — Qt6 desktop application; does not require Plasma.
- **ucc-cli** — command-line client.
- **ucc-gnome** — GNOME Shell panel extension, using Gio D-Bus directly.
- **ucc-tray** — optional KDE Plasma applet.

## Build and test

Requirements: CMake 3.20+, a C++20 compiler, pkg-config, Qt6 (Core, DBus, GUI,
Widgets, QML/Quick/QuickControls2, Bluetooth, Charts), systemd development files,
and the dependencies used by `uccd`. A working `tuxedo-drivers` installation is
required for real hardware control. Qt Wayland support is needed for native
Wayland sessions.

Tests additionally use Qt Test, Python 3, a C++ compiler, and `dbus-run-session`.
Install GJS and Node.js to include the GNOME client and extension tests; CMake
otherwise skips those tests. KDE Frameworks 6, ECM, and Plasma development files
are only required when `BUILD_TRAY=ON`.

### GNOME / Qt6 build without Plasma

```bash
git clone https://github.com/noteMASTER11/ucc.git
cd ucc
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
  -DBUILD_TRAY=OFF -DBUILD_GNOME=ON -DBUILD_TESTS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
python3 contrib/mechrevo/tests/test_migration.py
```

The daemon, GUI, and CLI are enabled by default. For the optional Plasma applet,
install its dependencies and use `-DBUILD_TRAY=ON`.

`cmake --install build` installs to the configured prefix and requires suitable
permissions. The Arch/CachyOS recipe below also installs the suspend unit used
on the validated machine. The generic CMake install retains upstream's separate
pre-sleep and resume units; it is not identical to that package.

### Arch / CachyOS packages and migration

See [the Mechrevo integration guide](contrib/mechrevo/README.md) for:

- Building UCC from the current checkout.
- Building the separate DKMS package from pinned TUXEDO drivers 4.23.0 sources.
- The exact-DMI, ITE RGB, and NVIDIA diagnostic-mode driver patches.
- Converting saved TCC profiles, fan curves, keyboard zones, and cooler identity.
- Service ordering, rollback preparation, and validation limits.

UCC itself does not supply or automatically upgrade the kernel driver. The
optional companion driver package keeps NVIDIA EC power writes disabled by
default; moving to UCC does not enable a writable cTGP interface.

### Other distributions and NixOS

Upstream distribution packaging (`make rpm`, `make deb`, `make arch`) and the Nix
flake are retained. Those paths have not been validated for this fork's complete
Mechrevo integration; in particular, they do not automatically install its
companion driver patches or Arch suspend-unit override.

```bash
nix build
nix develop
```

For NixOS, use `ucc.url = "github:noteMASTER11/ucc"`, import
`ucc.nixosModules.default`, and enable `services.uccd.enable`. The upstream
`services.uccd.package`, `extraArgs`, and `enableSleepHandler` options remain
available. See [flake.nix](flake.nix) and [nix/](nix/) for the configuration.

## Validation and remaining limits

The local Qt/GJS/Node test suite passed **16/16 tests** before publication. It
covers exact device identification, saved cooler selection, late Bluetooth
adapter startup, retry backoff, async D-Bus behavior, bounded slider queues,
hidden-menu polling, GUI singleton behavior, cooler-control side effects, and
headless monitoring. The tests use private buses and simulated services rather
than controlling the live cooler.

TUXEDO drivers 4.23.0 with the included patch set built through DKMS for
`7.2.6-1-cachyos` and `6.18.52-1-cachyos-lts`. A reboot confirmed the loaded driver
and automatic cooler connection. These are observations on one machine.

Remaining limitations:

- Slow EC/WMI operations still execute in the daemon. Async clients avoid waiting
  for them on the desktop UI thread, but do not make the hardware itself faster.
- Some initial GUI loads and explicit user commands remain synchronous.
- BLE command spacing still includes a short sleep in the daemon; this is not a
  complete conversion of hardware I/O to an asynchronous command queue.
- Suspend/resume, prolonged cooler absence, and focus behavior across desktop
  environments need further hardware validation.
- The launcher change is verified at the Gio metadata/launch boundary; visual
  confirmation that the busy cursor disappears is still pending. Elimination of
  all cursor stutters is not claimed.

## Credits and license

Based on [nanomatters/ucc](https://github.com/nanomatters/ucc), with TUXEDO IO code
from TUXEDO Control Center and hardware research from
[Mechrevo-Yaoshi-Linux](https://github.com/noteMASTER11/Mechrevo-Yaoshi-Linux).
UCC is GPL-3.0-or-later; the separate TUXEDO driver sources and patches retain
their upstream licensing.

## Screenshots

Upstream screenshots (not a claim that every pictured feature was validated on
Mechrevo):

![Dashboard](screenshots/1.png)
![Profiles](screenshots/2.png)
![Hardware controls](screenshots/3.png)
