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

Apply the driver patch from this directory before rebuilding DKMS:

```bash
cd /usr/src/tuxedo-drivers-4.21.0
sudo patch -p1 < /path/to/ucc/docs/mechrevo-yaoshi-x6ar55xy-tuxedo-drivers-4.21.0.patch
sudo dkms build --force -m tuxedo-drivers -v 4.21.0 -k "$(uname -r)"
sudo dkms install --force -m tuxedo-drivers -v 4.21.0 -k "$(uname -r)"
```

After reloading modules, UCC should report the Uniwill interface, model 26, three
ODM power limits, two fans, and 126 RGB keyboard backlight zones.
