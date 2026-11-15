# Chromebook SOF Audio for Windows

An open Windows 10/11 audio driver stack for Intel Gemini Lake Chromebooks
running MrChromebox UEFI firmware. It replaces the closed, per-device-licensed
Coolstar `csaudiointcsof` driver.

First targets: Google Octopus boards (Acer Chromebook 311 "Bobba", HP "Dorp"),
with a Celeron N4020, DA7219 headset codec and MAX98357A speaker amp.

| Folder | What it is |
|---|---|
| `drivers/sklhdaudbus` | HDA bus driver. Coolstar, BSD-3, unchanged (see `drivers/UPSTREAM.txt`) |
| `drivers/da7219` | DA7219 headset codec driver. Coolstar, Apache-2.0, unchanged |
| `drivers/max98357a` | MAX98357A amp driver. Coolstar, Apache-2.0, unchanged |
| `drivers/cbsof` | **New** SOF DSP driver (this project) |
| `drivers/cbsof/firmware` | Intel SOF v2.2 community firmware + topology (Intel license) |
| `docs/DESIGN.md` | Architecture, clean-room rules, milestones |

## Status

Milestone 2 (bring-up) **works on hardware** (Bobba, 2026-11-14): `cbsof.sys`
boots the SOF 2.2.0 firmware on the DSP and completes an IPC round trip. It has
no audio endpoints yet. It writes its log to the
registry:

```powershell
Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Enum\CSAUDIO\ADSP&CTLR_VEN_8086&CTLR_DEV_3198\*\Device Parameters" | Select-Object -ExpandProperty CbsofLog
```

## Building

Visual Studio 2022 with the WDK 10.0.26100, Release|x64. Test-sign the
packages, and run the target machine with test signing on.
