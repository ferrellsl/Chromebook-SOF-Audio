# Chromebook SOF Audio for Windows: design notes

Target: Gemini Lake Chromebooks on MrChromebox UEFI firmware. The first three
devices are Octopus boards: Bobba (x2) and Dorp.

| Part | Hardware ID | Driver |
|---|---|---|
| HDA controller + ADSP | `PCI\VEN_8086&DEV_3198` | `sklhdaudbus`, Coolstar, BSD-3, built from source |
| DSP child device | `CSAUDIO\ADSP&CTLR_VEN_8086&CTLR_DEV_3198` | **new: `cbsof.sys`** (this project) |
| Headset codec DA7219 | `ACPI\DLGS7219` | `da7219`, Coolstar, Apache-2.0, built from source |
| Speaker amp MAX98357A | `ACPI\MX98357A` | `max98357a`, Coolstar, Apache-2.0, built from source |
| DMIC array | (through the DSP) | handled by the SOF firmware |
| HDMI audio | `HDAUDIO\FUNC_01&VEN_8086&DEV_280D` | Microsoft inbox driver on top of `sklhdaudbus` |

## Clean-room rule

`cbsof.sys` replaces Coolstar's closed, per-device-licensed `csaudiointcsof.sys`.
The closed binary is never disassembled, patched or copied. Allowed sources:

- the Linux SOF host driver (`sound/soc/sof`, dual BSD-3/GPL-2), used under BSD-3
- the SOF firmware IPC3 headers (`thesofproject/sof` stable-v2.2, `src/include/ipc`, BSD-3)
- Coolstar's open-source repos, which have licenses (sklhdaudbus, da7219, max98357a)
- `coolstar/csaudiosstcatpt` has **no license**: read it to learn the shape of the
  interfaces, but don't copy any code from it
- Microsoft's sysvad / PortCls WaveRT samples (MIT)

## Firmware and topology

- `sof-glk.ri`: the community-signed SOF v2.2 image (the APL build). It is
  byte-identical to the file Coolstar ships
  (md5 1c991a6c34aa28f9af5cb9a7160e3992 = sof-bin 2026.09.2 `sof/community/sof-glk.ri`).
  Chromebooks verify DSP firmware against the community key.
- `sof-glk-da7219.tplg`: SOF topology v2.2.1. The same pipelines are used on Linux
  for these boards.

`sof-glk-da7219.tplg` contents, from a first look (ALSA topology, magic `CoSA`):

| DSP port | Pipeline / PCM | Endpoint |
|---|---|---|
| SSP1 (`SSP1-Codec`, `ssp1 Tx`) | Speakers (PCM 0) | MAX98357A speakers |
| SSP2 (`SSP2-Codec`, Tx + Rx) | Headset playback (PCM 1) + capture | DA7219 headphones + headset mic |
| DMIC0 (`dmic01`) | PCM 99 capture | built-in mic array |
| HDA3/4/5 (`iDisp1..3`) | PCM 5/6/7 | HDMI/DP, which we can leave to the HDA path |

## Interfaces `cbsof.sys` consumes

1. **ADSP bus interface** from `sklhdaudbus` (`GUID_ADSP_BUS_INTERFACE`, `adsp.h`).
   It provides the HDA and ADSP BARs, the PP capability pointer, NHLT, PCI config,
   DSP power, the interrupt callback, and HDA host-DMA streams (get, prepare,
   trigger, position, SPIB). We use host DMA both for code loading and for PCM.
2. **Topology hint**: `sklhdaudbus` reads ACPI `_DSD` keys `speaker-tplg`,
   `hp-tplg` and `mic-tplg` (`sof-tplg.cpp`). Still to check what MrChromebox
   puts there on Octopus.
3. **CsAudio callback API** (`\Callback\CsAudioCallbackAPI`, `CsAudioArg`): codec
   drivers register endpoints. The DSP driver tells them when to start and stop,
   and they report jack events through it.

## Boot sequence (from Linux `hda-dsp.c`, `hda-loader.c`, `apl.c`)

1. Power up core 0 (ADSPCS), wait for ROM init (HIPCIE / ROM status in the FW regs).
2. Code loader: allocate a host *playback* stream, copy `sof-glk.ri` minus the
   manifest into its BDL buffer, send the ROM the purge-FW IPC with the stream
   tag, run the stream, then poll ROM status until FW_ENTERED.
3. Wait for the `SOF_IPC_FW_READY` message, then read the mailbox layout
   (window regions from the ext data).
4. Parse the topology, then send IPC3 `TPLG_*_NEW` for each widget/route,
   pipeline complete, DAI config (SSP for DA7219/MAX98357A, DMIC).
5. PCM: `STREAM_PCM_PARAMS` with a host stream tag + page table, then
   `STREAM_TRIG_START`, and position from host DMA (or a stream position mailbox).

## Milestones

1. Toolchain: build sklhdaudbus, da7219, max98357a unchanged and install them on
   the test machine.
2. `cbsof.sys` binds to the CSAUDIO device, maps BARs, boots the DSP and gets
   FW_READY. Logged with WPP/DbgPrint.
3. Topology loaded, speakers play a tone.
4. PortCls WaveRT endpoints: Speakers, Headphones, Mic Array, Mic, with jack switching.
5. D3 / sleep / resume, polish, installer.

Test machine: one Bobba in test-signing mode (Secure Boot off).
