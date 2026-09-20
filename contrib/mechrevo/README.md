# Mechrevo YAOSHI integration

This directory preserves the companion work used with the exact
`MECHREVO / YAOSHI Series-X6AR55xY / Standard / 0001` DMI tuple. It contains source
patches and build recipes, not installed configurations or prebuilt packages.

## Arch/CachyOS builds

From a checkout of this repository, as a regular user:

```bash
cd contrib/mechrevo/arch/drivers
makepkg -s
```

The driver recipe fetches TUXEDO drivers commit
`2c6bf54075fb38a7fdbefc560734281984bf65bc` (4.23.0), verifies the local patch/config
checksums, and packages the patched sources for DKMS. It does not load modules or
change hardware. Matching kernel headers are needed when DKMS builds the modules
at package installation.

Build UCC separately:

```bash
cd contrib/mechrevo/arch/ucc
makepkg -s
```

Both `cd` paths above are relative to the repository root. UCC's recipe builds
the current checkout with `BUILD_TRAY=OFF`, runs CTest and migration-converter
tests, then packages the result. Its `tuxedo-drivers` dependency must be satisfied
before building; the companion package provides it. Installing packages and
enabling services are separate steps and should follow review of the generated
packages and existing machine configuration.

The UCC recipe replaces upstream's two sleep handlers with the included
`uccd-sleep.service`: it stops UCC before `sleep.target` and the NVIDIA sleep
services, then starts UCC when the sleep target is left. Do not enable the old
`uccd-pre-sleep.service` alongside this override.

## Driver changes

[mechrevo.patch](arch/drivers/mechrevo.patch) includes:

- A shared exact-DMI predicate used by the compatibility check, Uniwill feature
  selection, and the existing Stellaris Gen7 TDP mapping.
- The ITE 8291 RGB compatibility change required by this kernel setup.
- A narrowly matched NVIDIA power-control module: exact DMI and GPU/subsystem
  IDs, with `active_mode` disabled by default. In this mode the module does not
  expose writable power sysfs controls or issue power-control EC writes.

The `.conf.example` file is documentation only and is not installed in
`/etc/modprobe.d`. Enabling it is outside the validated migration. UCC reports
controller capabilities, but that does not establish that every corresponding
sysfs control is enabled or validated on this machine.

UCC and the driver use independent version histories. Installing this fork does
not by itself patch an existing `tuxedo-drivers` package.

## TCC configuration converter

`migrate-settings.py` is the one-way converter used for the audited LCT22002/TCC
configuration. It is deliberately limited: unsupported fan presets, unsupported
cooler identity, invalid saved addresses, an active cTGP interface, and the
unsupported 12 V pump setting are rejected. It is not a general TCC backup tool.

It reads a private baseline directory containing these JSON files:

- `tcc-settings.json`: TCC settings, including keyboard zones and state mapping.
- `tcc-profiles.json`: profile list.
- `tcc-user.json`: saved Aquaris identity and serialized `aquarisSaveState`.
- `balanced-fan.json`: the TCC Balanced CPU/GPU fan tables.

It also reads the current CPU EPP/HWP settings from sysfs. It preserves effective
fan curves after applying TCC offsets and limits, creates UCC custom keyboard and
fan profiles, maps cooler pump/fan settings, and copies saved cooler LED state.
The custom fan-profile label reflects the original Balanced +20% migration;
review the generated curves when using different inputs.

```bash
python3 contrib/mechrevo/migrate-settings.py /path/to/private-baseline /path/to/private-output
```

Output is `settings` plus `aquaris.json`, with mode 0600 in a directory created
with mode 0700. Use a new private output directory. The converter does not install
the files, alter live settings, or contact the cooler. Existing backup paths and
cooler addresses are not included in this repository.

The daemon reads optional `/etc/ucc/aquaris.json` for the saved cooler identity
and LED state. If that file exists but is invalid, automatic device selection
fails closed. Keep it private because it contains the device address.

## Applying and reverting a migration

Keep the original TCC package, its settings, and the previously working driver
package available before switching controllers. Only one controller daemon
should manage the machine: the UCC service conflicts with `tccd.service`, and the
validated setup also disabled the old TCC sleep handler.

Review converted profiles before deploying them. Updating a running installation
can leave the daemon and GNOME extension using their previously loaded code.
A planned reboot applies the driver, daemon, and GNOME changes together; disabling
and re-enabling the extension does not reliably reload imported JavaScript
modules in the current GNOME session.

Rollback consists of stopping/disabling the UCC service and sleep handler,
restoring the original TCC configuration and service state, and restoring the
previous driver package if required. This repository does not automate those
machine-specific steps or overwrite backups.

## Verification scope

The UCC tests use private D-Bus sessions and simulated hardware replies. The
standalone converter tests use synthetic data:

```bash
python3 contrib/mechrevo/tests/test_migration.py
```

On the original machine, DKMS builds passed for the CachyOS main and LTS kernels;
a subsequent reboot confirmed automatic cooler connection. Sleep/resume and
long-duration disconnect recovery remain to be validated. Avoid interpreting
unit tests as proof of compatibility with an untested laptop.
