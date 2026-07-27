# DIO SX1255 Repeater

Direct-I/O SX1255 repeater integration for SvxLink.

This directory is intended to be a standalone git repository.

Runtime dependencies:

- `svxlink-server`
- `libasound2`
- `python3`
- `systemd`
- Linux SPI and ALSA loopback support

Build/configuration helper:

- `dialog` is used by `./configure` for interactive callsign/password entry.
- Use `./configure --no-dialog ...` for unattended builds.

Installed components:

- `sx1255_repeater`: C daemon handling SX1255 SPI mode control, RF I/Q ALSA, RX DSP, TX DSP.
- `svx_sx1255_ptt_bridge.py`: PTY bridge translating SvxLink PTT commands to repeater TCP commands.
- `sx1255-repeater.service`: systemd unit for the repeater.
- `svx-ptt-bridge.service`: systemd unit for the PTY bridge.
- `svxlink.conf`: SvxLink configuration for loopback audio and PTY PTT.
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
sudo systemctl restart svxlink.service
```

The `svxlink` user must have permissions for `/dev/spidev0.0` and ALSA devices.

The Debian package asks for SvxLink callsigns, reflector address, and `AUTH_KEY`
with `dialog` on first install if the configuration still contains placeholders.
Run this to repeat the question flow:

```sh
sudo dpkg-reconfigure sx1255-fm-repeater
```

Git/source notes:

- Commit `configure`, `Makefile.in`, `config.mk.in`, `src/`, `scripts/`, `systemd/`, `config/`, `debian/`, and `doc/`.
- Do not commit generated `Makefile`, `config.mk`, or `build/`.
