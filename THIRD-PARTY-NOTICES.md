# Third-party notices

This project bundles or derives from the following. Each keeps its own license.

| Part | Where | License | File |
|---|---|---|---|
| sklhdaudbus (HDA bus driver), CoolStar | `drivers/sklhdaudbus`, unchanged | BSD-3-Clause | `drivers/sklhdaudbus/LICENSE.txt` |
| da7219 (headset codec driver), CoolStar | `drivers/da7219`, unchanged | Apache-2.0 | `drivers/da7219/LICENSE.txt` |
| max98357a (speaker amp driver), CoolStar | `drivers/max98357a`, unchanged | Apache-2.0 | `drivers/max98357a/LICENSE.txt` |
| SimpleAudioSample (PortCls sample), Microsoft | `drivers/cbsof/cbsof/pc` (modified) | MS-PL | `drivers/cbsof/cbsof/pc/LICENSE-MS-PL.txt` |
| SOF firmware `sof-glk.ri` and topology `sof-glk-da7219.tplg`, Intel / Sound Open Firmware (sof-bin v2026.09.2) | `drivers/cbsof/firmware` | SOF firmware release licence | `drivers/cbsof/firmware/LICENCE.Intel` |
| Linux SOF host driver (`sound/soc/sof`) and SOF IPC3 headers: the DSP boot, IPC and topology logic follows them | `drivers/cbsof/cbsof`, `tools` | BSD-3-Clause (dual BSD/GPL-2.0, used under BSD-3) | — |

Upstream revisions are listed in `drivers/UPSTREAM.txt` and
`drivers/cbsof/firmware/SOURCE.txt`.

This project does not contain, and was not derived from, CoolStar's closed
`csaudiointcsof` driver.
