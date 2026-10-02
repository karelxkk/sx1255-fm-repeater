# DIO SX1255 Repeater

Direct-I/O SX1255 repeater integration for SvxLink.

This directory is intended to be a standalone git repository.

## Installation Order

Install the hardware support first, then the repeater package:

1. Install the Raspberry Pi Zero 2 kernel package:
   `rpi-zero2-kernel-6.18.54-myzero2+.deb`
2. Install and enable the `genericstereoaudiocodec` device-tree overlay.
3. Reboot.
4. After the reboot, verify that ALSA sees the `GenericStereoAu` audio device.
5. Install `sx1255-fm-repeater_0.2.0_arm64.deb`.
6. Configure SvxLink/repeater values when the package asks through `dialog`.

Runtime dependencies:

- `svxlink-server`
- `libasound2`
- `libgpiod3`
- `python3`
- `python3-gpiod` (required for the optional external PA PTT output)
- `systemd`
- Linux SPI and ALSA loopback support
- Raspberry Pi kernel with the SX1255 I2S audio overlay

Build/configuration helper:

- `dialog` is used by `./configure` for interactive callsign/password entry.
- Use `./configure --no-dialog ...` for unattended builds.

Installed components:

- `sx1255_repeater`: C daemon handling SX1255 SPI mode control, RF I/Q ALSA, RX DSP, TX DSP.
- `svx_sx1255_ptt_bridge.py`: PTY bridge translating SvxLink PTT commands to repeater TCP commands.
- `sx1255-repeater.service`: systemd unit for the repeater.
- `svx-ptt-bridge.service`: systemd unit for the PTY bridge.
- `svxlink.service.d/sx1255-fm-repeater.conf`: systemd ordering drop-in so
  SvxLink starts after the PTY bridge.
- `svxlink.conf.in`: SvxLink configuration template for loopback audio and PTY PTT.
- `snd-aloop.conf`: module-load and modprobe configuration for two ALSA loopback cables.

Build and install from the source tree:

```sh
./configure
make
sudo make install
```

For non-interactive configuration:

```sh
./configure --repeater-callsign=YOURCALL-R --reflector-callsign=YOURCALL-L \
  --reflector-hosts=reflector.example.org:5300 --auth-key=YOUR_SECRET
```

Create a Debian package:

```sh
./configure
make deb
sudo apt install ./build/deb/sx1255-fm-repeater_*.deb
```

Enable services after reviewing `/etc/default/sx1255-repeater` and `/etc/svxlink/svxlink.conf`:

```sh
sudo systemctl enable --now sx1255-repeater.service
sudo systemctl enable --now svx-ptt-bridge.service
sudo systemctl enable --now svxlink
```

The package adds the `svxlink` user to the `audio`, `spi`, and `gpio` groups
when those groups exist.  The `svxlink` user must have permissions for
`/dev/spidev0.0`, `/dev/gpiochip0`, and ALSA devices.

The Debian package asks for SvxLink callsigns, reflector address, and `AUTH_KEY`
with `dialog` on first install if the configuration still contains placeholders.
When it writes `/etc/svxlink/svxlink.conf`, the previous file is kept as
`/etc/svxlink/svxlink.conf.before-sx1255-fm-repeater`.
Run this to repeat the question flow:

```sh
sudo dpkg-reconfigure sx1255-fm-repeater
```

## Raspberry Pi Zero 2 Kernel And Overlay

The release provides these support files for Raspberry Pi Zero 2:

- `rpi-zero2-kernel-6.18.36-myzero2+.deb`
- `genericstereoaudiocodec.dtbo`

Step 1: install the kernel package first:

```sh
sudo apt install ./rpi-zero2-kernel-6.18.36-myzero2+.deb
```

Step 2: install the device-tree overlay:

```sh
sudo install -m 0644 genericstereoaudiocodec.dtbo /boot/firmware/overlays/genericstereoaudiocodec.dtbo
```

Enable SPI, I2S, and the overlay in `/boot/firmware/config.txt`:

```ini
dtparam=spi=on
dtparam=i2s=on
dtoverlay=genericstereoaudiocodec
```

On older Raspberry Pi OS images the config file may be `/boot/config.txt`
instead of `/boot/firmware/config.txt`.

Step 3: reboot:

```sh
sudo reboot
```

Step 4: after the system starts again, verify that ALSA sees the SX1255 audio
device:

```sh
aplay -l
arecord -l
```

The repeater defaults expect the card name used by this overlay:

```sh
PLAYBACK_DEV=hw:GenericStereoAu,0,0
CAPTURE_DEV=hw:GenericStereoAu,1,0
```

If ALSA assigns a different card name or index, override these values in
`/etc/default/sx1255-repeater`.

## Disable Wi-Fi Power Save

On Raspberry Pi OS Lite it is recommended to disable Wi-Fi power saving, since
power save can add latency or cause unreliable reflector/network behavior.

For a temporary test until the next reboot:

```sh
sudo iw dev wlan0 set power_save off
iw dev wlan0 get power_save
```

For a persistent NetworkManager configuration:

```sh
sudo install -d /etc/NetworkManager/conf.d
printf '[connection]\nwifi.powersave = 2\n' | sudo tee /etc/NetworkManager/conf.d/wifi-powersave-off.conf
sudo systemctl restart NetworkManager
```

After reconnecting Wi-Fi, verify:

```sh
iw dev wlan0 get power_save
```

Step 5: install the repeater package only after the kernel and overlay are
active.  The `libgpiod3` runtime library is pulled automatically as a package
dependency:

```sh
sudo apt install ./sx1255-fm-repeater_0.2.0_arm64.deb
```

Enable the repeater, PTT bridge, and SvxLink services:

```sh
sudo systemctl enable --now sx1255-repeater.service
sudo systemctl enable --now svx-ptt-bridge.service
sudo systemctl enable --now svxlink
```

## External Power Amplifier (PA PTT)

The PTT bridge can key an external power amplifier through a spare GPIO line
in addition to switching the SX1255 between RX and DUP.  The feature is
disabled by default and has no effect unless `PA_PTT_ENABLED=1`.

### Pin selection

- Recommended pin: **GPIO17** (physical pin 11).
- Do **not** use: GPIO22 (SX1255 reset), GPIO18-21 (I2S audio), GPIO8-11
  (SPI0), GPIO14/15 (UART console).
- Verify availability on your hardware with `gpioinfo` or `raspi-gpio get`.

### Configuration

Set the following environment variables in
`/etc/systemd/system/svx-ptt-bridge.service` (or the unit template
`systemd/svx-ptt-bridge.service.in`):

| Variable              | Meaning                                                  |
|-----------------------|----------------------------------------------------------|
| `PA_PTT_ENABLED`      | `1` enables PA PTT, `0` disables it (default)            |
| `PA_PTT_CHIP`         | GPIO chip, usually `/dev/gpiochip0`                      |
| `PA_PTT_LINE`         | BCM GPIO number to use                                   |
| `PA_PTT_ACTIVE_LEVEL` | `1` = active high, `0` = active low                      |
| `PA_PTT_PRE_MS`       | delay after keying the PA before TX, in milliseconds     |
| `PA_PTT_POST_MS`      | delay after returning to RX before unkeying the PA, in ms |

The bridge keys the PA *before* switching the radio to TX and unkeys it
*after* switching back to RX, which protects the amplifier from hot
switching.  If switching to TX fails, the PA is unkeyed immediately.

After changing the unit file, reload and restart:

```sh
sudo systemctl daemon-reload
sudo systemctl restart svx-ptt-bridge.service
journalctl -u svx-ptt-bridge -f
```

### Electrical wiring

Always drive the PA key line through a transistor or optocoupler.  Do not
connect a relay coil directly to a GPIO pin; the pin cannot supply the
required current and the back-EMF can damage the SoC.

## SX1255 Frequencies

The default SX1255 startup frequencies are set in the systemd unit template:

```ini
ExecStartPre=+@SPIBINDIR@/sx1255-spi 430950020 438575000
```

The nominal RX channel is `430975020` Hz including the `20` Hz correction.  The
SX1255 RX startup frequency is intentionally configured `25 kHz` lower, i.e.
two 12.5 kHz channels below the wanted receive frequency:

```text
wanted RX:       430975020 Hz
configured RX:   430950020 Hz
offset:             -25000 Hz
```

The TX startup frequency is:

```text
configured TX:   438575000 Hz
```

To change these values, edit the `ExecStartPre` line in
`sx1255-repeater.service` and reload systemd:

```sh
sudo systemctl edit --full sx1255-repeater.service
sudo systemctl daemon-reload
sudo systemctl restart sx1255-repeater.service
```

Transmit audio must include CTCSS `88.5 Hz`.

On my reflector setup, the expected SvxLink talk group is `123`.

To change CTCSS or talk group settings, edit `/etc/svxlink/svxlink.conf`:

```sh
sudo nano /etc/svxlink/svxlink.conf
```

Git/source notes:

- Commit `configure`, `Makefile.in`, `config.mk.in`, `src/`, `scripts/`, `systemd/`, `config/`, `debian/`, and `doc/`.
- Do not commit generated `Makefile`, `config.mk`, or `build/`.
