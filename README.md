<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/icon-dark.png">
    <img src="docs/icon.png" width="128" height="128" alt="Tether app icon">
  </picture>
</p>

# tetherd

Android USB tethering (RNDIS) for macOS, done entirely in userspace, with
**Tether**, a menu bar app to switch it on and off.

<p align="center">
  <img src="docs/panel.png" width="322" alt="Tether's Liquid Glass panel, connected to a Redmi Note 13 NFC: interface feth0, address 10.191.118.213, the &quot;Use phone as main connection&quot; switch and a Disconnect button">
</p>

`tetherd` is a small root daemon. It needs no kernel extension, no DriverKit
extension, no Apple Developer account, no code signature, and no change to SIP
or to the boot loader. It talks to the phone with the ordinary userspace USB
API and hands the traffic to macOS through a virtual Ethernet interface.

It is the userspace sibling of [TetherKit](https://github.com/Gyuri1999/TetherKit)
(DriverKit extension, and a kext for OpenCore) and shares its RNDIS protocol
code, which descends from [HoRNDIS](https://github.com/jwise/HoRNDIS).

## How it works

```
phone <--USB--> tetherd <--BPF--> fethB ==== fethA <--> macOS network stack
     (IOUSBLib, RNDIS)          (I/O side)  (host side: DHCP, ARP, IPv6)
```

- **USB side.** `IOUSBLib` opens the phone's RNDIS control interface and the
  CDC data interface next to it. RNDIS commands go over the control pipe;
  Ethernet frames, each wrapped in an RNDIS data message, go over the two bulk
  pipes.
- **Network side.** A process cannot register a real Ethernet controller, so
  the daemon creates a pair of `feth` ("fake Ethernet") interfaces, which act
  like two ports joined by a cable. macOS uses one end as a normal Ethernet
  interface. The daemon attaches a BPF device to the other end and copies
  frames in both directions.

Because the host end is real Ethernet as far as macOS is concerned, macOS does
DHCP, ARP and IPv6 itself; the daemon only moves frames.

## Build

```sh
make
```

Needs the Xcode command line tools. Binaries are universal (Intel and Apple
silicon); to build for this Mac only: `make ARCHS=$(uname -m)`.

## The app

`Tether.app` is a menu bar app around the daemon: open it when you want to
tether, quit it when you are done.

```sh
make app             # builds build/Tether.app
make install-app     # copies it to /Applications
```

Then open **Tether** from Applications, Spotlight or Launchpad. It has no Dock
icon; it lives in the menu bar. Opening it starts `tetherd`, which waits for
the phone, so plug the phone in and switch on USB tethering before or after.

Click the menu bar icon for the Liquid Glass panel: the state (waiting,
linking, online, error), the phone, the network interface and its address,
a **Connect**/**Disconnect** button, and buttons for verbose logging, the log,
removing the helper, and quitting. While the phone is connecting and macOS is
waiting for an address, the menu bar icon pulses. Quitting stops tethering and
removes the interface. If the app quits or crashes, `tetherd` notices that its
standard input has closed and shuts down cleanly too.

The app needs macOS 26 or later; `tetherd` on its own still runs on macOS 11.
The icon is an Icon Composer document, `app/Tether.icon`; `make app` renders
it with Icon Composer's `ictool` when Icon Composer is installed, and draws a
flat version otherwise.

The first time it runs (and after you install a newer build) it asks once for
an administrator password and installs:

- `/Library/PrivilegedHelperTools/tetherd`, a root-owned copy of the daemon;
- `/etc/sudoers.d/tetherd`, a rule that lets your user run exactly that binary
  as root without a password (checked with `visudo` before it is installed).

After that it starts without prompting. **Remove Helper…** in the menu deletes
both. The output goes to `~/Library/Logs/Tether.log` (**Open Log**); turn on
**Verbose Logging** in the menu when debugging. If the boot-time daemon from
`sudo make install` is present, the app offers to remove it, since the two
would compete for the phone.

## Use from the command line

Plug the phone in, switch on USB tethering, then:

```sh
sudo ./build/tetherd
```

It logs the name of the interface it created (for example `feth0`) and asks
macOS to configure it by DHCP. Stop it with Ctrl-C; the interface is removed.

| Option | |
|---|---|
| `-l`, `--list` | List attached RNDIS devices and exit. No root needed. |
| `-p`, `--probe` | As `--list`, and also initialise each device and read its address, without touching networking. |
| `-n`, `--no-dhcp` | Create the interface but leave address configuration to you. |
| `-v`, `--verbose` | Log more detail. |
| `--stdin-eof` | Exit cleanly when standard input is closed. Used by the app. |

To run it at boot as a launchd daemon, so tethering starts whenever the phone
is plugged in:

```sh
sudo make install
```

```sh
sudo make uninstall
```

The daemon logs to `/var/log/tetherd.log`.

## Limits

- **Root is required**, to create interfaces and open BPF.
- **Not in System Settings.** `feth` interfaces do not appear under
  System Settings → Network. Check the connection in Tether's panel or with
  `ifconfig feth0`.
- **One driver at a time.** If TetherKit.kext, the TetherKit DriverKit
  extension or HoRNDIS is loaded, it claims the phone first and `tetherd`
  reports the interface as in use. Use one or the other.
- **One phone at a time.**
- **Slower than a kernel driver.** Every frame is copied through BPF and one
  frame is sent per USB transfer (with several transfers in flight). Fine at
  mobile network speeds.
- **IPv4 only as main connection.** `--primary` promotes the phone's IPv4
  service; if Wi-Fi has IPv6 and the phone does not, IPv6 traffic can still
  use Wi-Fi.
- **Composite devices only.** The phone must present RNDIS as an interface of a
  composite USB device, which is what Android does. The rare device-level
  variant (device class 224/0/0, some older Samsung phones) is not handled.
- **No RNDIS keep-alive**, and the control channel is polled rather than
  waiting on the interrupt endpoint, as in HoRNDIS.

## Status

Works on macOS 27 with a Redmi Note 13 NFC and a Redmi Note 14 5G, through
both the command line and `Tether.app`:

- the phone hands out an address over DHCP within a few seconds (some phones
  ignore the first request on a new link; `tetherd` asks again after 4 s);
- traffic and DNS flow through the phone, and with `--primary` it stays the
  Mac's main connection with Wi-Fi on, handing back to Wi-Fi on disconnect;
- measured throughput, about 4 MB/s (33 Mbit/s) down and up, matched a speed
  test run on the phone itself, so the mobile network was the limit; the
  daemon used 10-20 % of one core at that rate.

Not yet tried: the Intel build on a real Intel Mac (it runs under Rosetta),
and phones other than these two.

## Layout

| | |
|---|---|
| `src/main.cpp` | Device discovery, session lifecycle, the two copy threads. |
| `src/RNDISDevice.*` | USB and the RNDIS control protocol. |
| `src/FakeEthernet.*` | The `feth` pair and BPF. |
| `src/RNDISProtocol*.{h,cpp}` | RNDIS structures and framing, shared with TetherKit. |
| `launchd/` | The launchd daemon definition used by `make install`. |
| `app/main.swift` | `Tether.app`: helper setup, running `tetherd`, the menu bar item. |
| `app/Panel.swift` | The app's Liquid Glass panel (SwiftUI). |
| `app/Tether.icon` | The app icon, an Icon Composer document. |
| `app/MakeIcon.swift` | Renders the icon into an `.icns` at build time. |

## License

GPL-3.0-or-later, as TetherKit. RNDIS logic derives from HoRNDIS
(Joshua Wise, Mikhail Iakhiaev) and Linux `rndis_host.c` (David Brownell).
