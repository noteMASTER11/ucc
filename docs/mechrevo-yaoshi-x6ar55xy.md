# Mechrevo YAOSHI Series X6AR55xY

Hardware observed on the tested machine:

- DMI sys vendor: `MECHREVO`
- DMI product name: `YAOSHI Series`
- DMI board name: `YAOSHI Series-X6AR55xY`
- WMI ACPI device: `PNP0C14:02` at `_SB_.AMW0`
- Uniwill WMI GUIDs: `ABBC0F6A-8EA1-11D1-00A0-C90629100000` through `ABBC0F72-8EA1-11D1-00A0-C90629100000`
- Keyboard HID product: `048D:600B`

This machine uses the same Uniwill/TUXEDO driver path as the X6AR55xU family,
but `tuxedo-drivers 4.21.0` rejects the Mechrevo DMI strings and does not attach
the TDP definitions without an extra board match.

The patch also exposes the NVIDIA Dynamic Boost/cTGP sysfs controls that are
hidden behind `#ifdef DEBUG` in `tuxedo_nb02_nvidia_power_ctrl`. Without those
extra nodes, UCC can write `ctgp_offset`, but cannot force-refresh
`ctgp_enable`, `db_enable`, `tpp_offset`, and `db_offset` when the EC/NVIDIA
driver keeps the active dGPU power limit pinned to the default value. The patch
also changes the module init value for `ctgp_offset` from `0` to raw `255` so
the EC boots into the most aggressive exposed Dynamic Boost/cTGP state before
UCC starts enforcing the same state.

Apply the driver patch from this directory before rebuilding DKMS:

```bash
cd /usr/src/tuxedo-drivers-4.21.0
sudo patch -p1 < /path/to/ucc/docs/mechrevo-yaoshi-x6ar55xy-tuxedo-drivers-4.21.0.patch
sudo dkms build --force -m tuxedo-drivers -v 4.21.0 -k "$(uname -r)"
sudo dkms install --force -m tuxedo-drivers -v 4.21.0 -k "$(uname -r)"
```

After reloading modules, UCC should report the Uniwill interface, model 26, three
ODM power limits, two fans, 126 RGB keyboard backlight zones, and the following
GPU power-control nodes:

```bash
/sys/devices/platform/tuxedo_nvidia_power_ctrl/ctgp_offset
/sys/devices/platform/tuxedo_nvidia_power_ctrl/ctgp_enable
/sys/devices/platform/tuxedo_nvidia_power_ctrl/db_enable
/sys/devices/platform/tuxedo_nvidia_power_ctrl/tpp_offset
/sys/devices/platform/tuxedo_nvidia_power_ctrl/db_offset
```

UCC's Mechrevo aggressive dGPU path now keeps `ctgp_offset=255`, `ctgp_enable=1`,
`db_enable=1`, `tpp_offset=255`, `db_offset=25`, forces the Uniwill `overboost`
profile, and writes the maximum reported ODM TDP values while NVIDIA cTGP is
available.
