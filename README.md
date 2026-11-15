# Chromebook SOF Audio for Windows

Open Windows 10/11 audio drivers for Intel **Gemini Lake** Chromebooks running
[MrChromebox](https://mrchromebox.tech) UEFI firmware. No license key, no
per-device activation: the whole stack is open source.

**Works:** speakers, headphones, the headset microphone and the built-in
microphones, with automatic switching when a headset is plugged in or out.

## Supported hardware

Google **Octopus** boards with the DA7219 headset codec and MAX98357A speaker
amp, for example:

| Board | Chromebook | Status |
|---|---|---|
| Bobba | Acer Chromebook 311 (C733) | tested (all features) |
| Dorp | HP Chromebook 14 G6 | tested (all features) |

Other Gemini Lake Chromebooks (Celeron N4000/N4020/N4100/N4120, Pentium
Silver N5000/N5030) with the same codec and amp should work too, but are
untested. The installer refuses to run on hardware without the Gemini Lake
audio DSP (PCI `8086:3198`).

## Install

1. In the MrChromebox firmware setup, make sure **Secure Boot is off**.
2. Download `Chromebook-SOF-Audio-Setup-x.y.z.exe` from
   [Releases](https://github.com/ferrellsl/Chromebook-SOF-Audio/releases) and run it.
3. Restart when asked.

The drivers are **test-signed**, so the installer turns on Windows test
signing (`bcdedit /set testsigning on`) and trusts this project's test
certificate. Windows then shows "Test Mode" on the desktop.

If CoolStar's audio driver is installed, the installer takes over the audio
DSP device from it; uninstalling (Settings > Apps) gives it back. The bus and
codec drivers are CoolStar's open-source ones, built from source here; where
CoolStar's own signed copies are already installed, Windows keeps using those.

A zip with the same files is on the Releases page too. From an administrator
PowerShell in the unpacked folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-on-chromebook.ps1 -Setup
```

## Troubleshooting

From an administrator PowerShell in `C:\Program Files\Chromebook SOF Audio`:

```powershell
powershell -ExecutionPolicy Bypass -File .\get-3-logs.ps1
```

writes `log.txt` (the driver's own log) and a few other files to the Desktop.
Please attach them to an issue.

## How it works

`cbsof.sys` replaces CoolStar's closed `csaudiointcsof.sys` on the audio DSP
device. It boots Intel's Sound Open Firmware (SOF v2.2, community-signed, as
used by Linux) on the DSP, sets up the same pipelines Linux uses for this
hardware, and presents them to Windows as PortCls WaveRT endpoints. Jack
detection comes from the DA7219 driver's HID reports. Details are in
[docs/DESIGN.md](docs/DESIGN.md).

| Folder | What it is |
|---|---|
| `drivers/cbsof` | the SOF DSP driver (this project) |
| `drivers/cbsof/firmware` | Intel SOF firmware + topology, and the pipeline files generated from it |
| `drivers/sklhdaudbus`, `drivers/da7219`, `drivers/max98357a` | CoolStar's open-source bus and codec drivers, unchanged |
| `tools` | `tplg2ipc.py`: turns the SOF topology into the IPC messages the driver sends |
| `installer`, `scripts` | installer, install script, packaging |

## Building

Visual Studio 2022 with the WDK 10.0.26100, and Inno Setup 6 for the
installer:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package.ps1
```

builds all four drivers (Release|x64), test-signs them with a local
certificate (created on first run), and writes the installer and zip to
`out\release`.

## License

MIT for this project's own code (see [LICENSE](LICENSE)). Bundled parts keep
their own licenses; see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

This is an independent project, not affiliated with Google, Intel or CoolStar.
