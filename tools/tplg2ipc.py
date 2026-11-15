"""Turn an SOF IPC3 topology into the exact IPC3 messages Linux would send.

The driver replays the messages instead of parsing the topology in the
kernel. The logic mirrors Linux sound/soc/sof/ipc3-topology.c, topology.c and
sof-audio.c (sof_ipc3_set_up_all_pipelines), used under BSD-3-Clause.

  python tools/tplg2ipc.py <in.tplg> <out.bin> [--pipes 1,2,3,4] [--list]

Output format, little-endian:
  "CBTP" u32 version(1) u32 n_msgs u32 n_pcms
  n_pcms x { u32 pcm_id, u32 direction(0 play,1 capture), u32 host_comp_id,
             u32 pipeline_id, char name[32] }
  n_msgs x { u32 length, bytes[length] }   (length is a multiple of 4)
"""
import argparse
import math
import struct
import sys

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tplg import Topology  # noqa: E402

# token ids (include/uapi/sound/sof/tokens.h)
TKN_BUF_SIZE, TKN_BUF_CAPS, TKN_BUF_FLAGS = 100, 101, 102
TKN_DAI_TYPE, TKN_DAI_INDEX, TKN_DAI_DIRECTION = 154, 155, 156
TKN_SCHED_PERIOD, TKN_SCHED_PRIORITY, TKN_SCHED_MIPS, TKN_SCHED_CORE, \
    TKN_SCHED_FRAMES, TKN_SCHED_TIME_DOMAIN, TKN_SCHED_DYNAMIC = 200, 201, 202, 203, 204, 205, 206
TKN_VOLUME_RAMP_TYPE, TKN_VOLUME_RAMP_MS = 250, 251
TKN_COMP_PERIOD_SINK, TKN_COMP_PERIOD_SOURCE, TKN_COMP_FORMAT, TKN_COMP_CORE_ID, TKN_COMP_UUID = \
    400, 401, 402, 404, 405
TKN_PCM_DMAC_CONFIG = 353
TKN_DMIC_DRIVER_VERSION, TKN_DMIC_CLK_MIN, TKN_DMIC_CLK_MAX, TKN_DMIC_DUTY_MIN, TKN_DMIC_DUTY_MAX,     TKN_DMIC_NUM_PDM_ACTIVE, TKN_DMIC_SAMPLE_RATE, TKN_DMIC_FIFO_WORD_LENGTH, TKN_DMIC_UNMUTE_RAMP_TIME_MS =     600, 601, 602, 603, 604, 605, 608, 609, 610
TKN_DMIC_PDM_TOKENS = (700, 701, 702, 703, 704, 705, 706)   # ctrl id, mic a/b enable, polarity a/b, clk edge, skew
DMIC_NUM_CTRL = 4          # SOF_DAI_INTEL_DMIC_NUM_CTRL
DMIC_MAX_PDMS = None       # --dmic-pdms
TKN_SSP_CLKS_CONTROL, TKN_SSP_MCLK_ID, TKN_SSP_SAMPLE_BITS, TKN_SSP_FRAME_PULSE_WIDTH, \
    TKN_SSP_QUIRKS, TKN_SSP_TDM_PADDING, TKN_SSP_BCLK_DELAY = 500, 501, 502, 503, 504, 505, 506

# IPC3 commands
def GLB(x): return x << 28
def CMD(x): return x << 16
TPLG_MSG, COMP_MSG, DAI_MSG = GLB(3), GLB(5), GLB(8)
COMP_NEW, COMP_CONNECT, PIPE_NEW, PIPE_COMPLETE, BUFFER_NEW = CMD(1), CMD(3), CMD(0x10), CMD(0x13), CMD(0x20)
COMP_SET_VALUE = CMD(1)
DAI_CONFIG = CMD(1)

COMP_HOST, COMP_DAI, COMP_VOLUME, COMP_BUFFER = 1, 2, 5, 12
DAI_TYPES = {"SSP": 1, "DMIC": 2, "HDA": 3, "ALH": 4}
FRAME_FMTS = {"s16le": 0, "s24le": 1, "s32le": 2, "float": 3}
DAI_CONFIG_SIZE = 120      # Linux sizeof(struct sof_ipc_dai_config): 52 + 68-byte union
DAI_FLAGS_HW_PARAMS = 1
VOLUME_FWL = 16
SOF_DAI_FMT_CBP_CFP, SOF_DAI_FMT_CBC_CFP, SOF_DAI_FMT_CBP_CFC, SOF_DAI_FMT_CBC_CFC = 0 << 12, 2 << 12, 3 << 12, 4 << 12
SOF_DAI_FMT_NB_NF, SOF_DAI_FMT_NB_IF, SOF_DAI_FMT_IB_NF, SOF_DAI_FMT_IB_IF = 0, 2 << 8, 3 << 8, 4 << 8

W_AIF_IN, W_AIF_OUT, W_DAI_IN, W_DAI_OUT, W_BUFFER, W_SCHED, W_PGA = 11, 12, 13, 14, 16, 17, 4


def tok(w, token, default=None):
    for t in w.tuples:
        if t.token == token:
            return t.value
    return default


def frame_fmt(w):
    v = tok(w, TKN_COMP_FORMAT)
    return FRAME_FMTS.get(v, 0) if v is not None else 0


def comp_config(w):
    # struct sof_ipc_comp_config: hdr(size,cmd) periods_sink periods_source reserved1 frame_fmt xrun reserved[2]
    return struct.pack("<II5I2I", 36, 0, tok(w, TKN_COMP_PERIOD_SINK, 0), tok(w, TKN_COMP_PERIOD_SOURCE, 0),
                       0, frame_fmt(w), 0, 0, 0)


def comp_msg(w, comp_type, body):
    """sof_comp_alloc: sof_ipc_comp header + body + optional 16-byte UUID."""
    uuid = tok(w, TKN_COMP_UUID)
    ext = uuid if uuid is not None and any(uuid) else b""
    size = 28 + len(body) + len(ext)
    hdr = struct.pack("<II5I", size, TPLG_MSG | COMP_NEW, w.comp_id, comp_type, w.index,
                      tok(w, TKN_COMP_CORE_ID, 0), len(ext))
    return hdr + body + ext


def vol_gain(value, tlv):
    tmin, step, mute = struct.unpack("<iii", struct.pack("<III", *tlv))
    if value == 0 and mute:
        return 0
    db = (tmin + value * step) / 100.0          # tlv is in 0.01 dB
    return int(round(10 ** (db / 20.0) * (1 << VOLUME_FWL)))


def build(t, pipes):
    # comp ids: every widget in load order (topology.c sof_widget_ready)
    next_id = 0
    by_name = {}
    for w in t.widgets:
        w.comp_id = next_id
        next_id += 1
        by_name[w.name] = w
    sel = [w for w in t.widgets if w.index in pipes]
    order = list(reversed(sel))        # Linux list_add() prepends to widget_list

    msgs = []
    for w in order:
        if w.id == W_SCHED:
            sched = by_name[w.sname]
            msgs.append((w.name, struct.pack("<II10I", 48, TPLG_MSG | PIPE_NEW, w.comp_id, w.index, sched.comp_id,
                                             tok(w, TKN_SCHED_CORE, 0), tok(w, TKN_SCHED_PERIOD, 0),
                                             tok(w, TKN_SCHED_PRIORITY, 0), tok(w, TKN_SCHED_MIPS, 0),
                                             tok(w, TKN_SCHED_FRAMES, 0), 0, tok(w, TKN_SCHED_TIME_DOMAIN, 0))))
        elif w.id == W_BUFFER:
            msgs.append((w.name, struct.pack("<II5I4I", 44, TPLG_MSG | BUFFER_NEW, w.comp_id, COMP_BUFFER, w.index,
                                             tok(w, TKN_COMP_CORE_ID, 0), 0,
                                             tok(w, TKN_BUF_SIZE, 0), tok(w, TKN_BUF_CAPS, 0), tok(w, TKN_BUF_FLAGS, 0), 0)))
        elif w.id in (W_AIF_IN, W_AIF_OUT):
            direction = 1 if w.id == W_AIF_OUT else 0
            body = comp_config(w) + struct.pack("<III", direction, 0, tok(w, TKN_PCM_DMAC_CONFIG, 0))
            msgs.append((w.name, comp_msg(w, COMP_HOST, body)))
        elif w.id == W_PGA:
            ctl = next((c for c in w.controls if c.kind == "mixer" and c.max > 1), None)
            channels = ctl.num_channels if ctl else 0
            vmin = vol_gain(0, ctl.tlv) if ctl and ctl.tlv else 0
            vmax = vol_gain(ctl.max, ctl.tlv) if ctl and ctl.tlv else 0
            body = comp_config(w) + struct.pack("<5I", channels, vmin, vmax, tok(w, TKN_VOLUME_RAMP_TYPE, 0),
                                                tok(w, TKN_VOLUME_RAMP_MS, 0))
            msgs.append((w.name, comp_msg(w, COMP_VOLUME, body)))
            if ctl:
                # sof_ipc3_widget_kcontrol_setup: set the volume to 0 dB on all channels
                n = ctl.num_channels
                size = 92 + 8 * n          # sizeof(struct sof_ipc_ctrl_data) = 92
                cdata = struct.pack("<IIi", size, COMP_MSG | COMP_SET_VALUE, 0)     # rhdr
                cdata += struct.pack("<IIII", w.comp_id, 1, 0, 0)                  # comp_id, type CHAN_SET, cmd VOLUME, index
                cdata += struct.pack("<I6I", 0, 0, 0, 0, 0, 0, 0)                  # host buffer (unused)
                cdata += struct.pack("<III", n, 0, 0) + b"\0" * 24                 # num_elems, remaining, msg_index, reserved
                for ch in range(n):
                    cdata += struct.pack("<II", ch, 1 << VOLUME_FWL)
                assert len(cdata) == size
                msgs.append((w.name + " volume 0dB", cdata))
        elif w.id in (W_DAI_IN, W_DAI_OUT):
            dai_type = DAI_TYPES[tok(w, TKN_DAI_TYPE)]
            dai_index = tok(w, TKN_DAI_INDEX, 0)
            body = comp_config(w) + struct.pack("<IIII", tok(w, TKN_DAI_DIRECTION, 0), dai_index, dai_type, 0)
            msgs.append((w.name, comp_msg(w, COMP_DAI, body)))
            cfg = dai_config(t, w, dai_type, dai_index)
            if cfg:
                msgs.append((w.name + " dai_config", cfg))

    # routes between a buffer and a component, reversed like route_list
    for r in reversed([r for r in t.routes if r.index in pipes]):
        src, sink = by_name[r.source], by_name[r.sink]
        if src.id != W_BUFFER and sink.id != W_BUFFER:
            continue
        msgs.append(("route %s -> %s" % (src.name, sink.name),
                     struct.pack("<IIII", 16, TPLG_MSG | COMP_CONNECT, src.comp_id, sink.comp_id)))

    for w in order:
        if w.id == W_SCHED:
            msgs.append(("complete " + w.name, struct.pack("<III", 12, TPLG_MSG | PIPE_COMPLETE, w.comp_id)))

    pcms = []
    for p in t.pcms:
        for direction, suffix in ((0, "P"), (1, "C")):
            if (direction == 0 and not p.playback) or (direction == 1 and not p.capture):
                continue
            host = next((w for w in sel if w.id in (W_AIF_IN, W_AIF_OUT) and
                         w.sname == ("Passthrough %s %d" % ("Playback" if direction == 0 else "Capture", p.pcm_id))), None)
            if host:
                pcms.append((p.pcm_id, direction, host.comp_id, host.index, p.name))
    return msgs, pcms


def dmic_config(link, hw, dai_index, max_pdms=None):
    """Linux ipc3-topology.c sof_link_dmic_load: hdr.size = sizeof(struct
    sof_ipc_dai_config) with room for SOF_DAI_INTEL_DMIC_NUM_CTRL PDM controllers."""
    params = struct.pack("<I", 0)                                   # hdr
    params += struct.pack("<5I", tok_link(link, TKN_DMIC_DRIVER_VERSION, 0), tok_link(link, TKN_DMIC_CLK_MIN, 0),
                          tok_link(link, TKN_DMIC_CLK_MAX, 0), tok_link(link, TKN_DMIC_SAMPLE_RATE, 0), 0)
    params += struct.pack("<4H", tok_link(link, TKN_DMIC_FIFO_WORD_LENGTH, 0), 0,
                          tok_link(link, TKN_DMIC_DUTY_MIN, 0), tok_link(link, TKN_DMIC_DUTY_MAX, 0))
    num_pdm = tok_link(link, TKN_DMIC_NUM_PDM_ACTIVE, 0)
    if max_pdms:
        # e.g. Bobba: only PDM0 has mics; PDM1 returns a constant, which
        # halves the level once Windows mixes the channels down.
        num_pdm = min(num_pdm, max_pdms)
    params += struct.pack("<I3I5I", num_pdm, 0, 0, tok_link(link, TKN_DMIC_UNMUTE_RAMP_TIME_MS, 0), 0, 0, 0, 0, 0)
    assert len(params) == 68
    # PDM token sets: the i-th occurrence of each token belongs to controller i
    pdms = [[0] * 7 for _ in range(DMIC_NUM_CTRL)]
    seen = {}
    for x in link.tuples:
        if x.token in TKN_DMIC_PDM_TOKENS:
            i = seen.get(x.token, 0)
            seen[x.token] = i + 1
            if i < num_pdm:
                pdms[i][TKN_DMIC_PDM_TOKENS.index(x.token)] = x.value
    assert 1 <= num_pdm <= DMIC_NUM_CTRL and all(seen.get(t, 0) >= num_pdm for t in TKN_DMIC_PDM_TOKENS[:3])
    assert all(pdms[i][0] == i for i in range(num_pdm))     # controller ids in order
    for pd in pdms:
        params += struct.pack("<I7H3H", 0, *pd, 0, 0, 0)
    size = 52 + len(params)
    msg = struct.pack("<IIIIHBB", size, DAI_MSG | DAI_CONFIG, 2, dai_index, hw.fmt, 0, DAI_FLAGS_HW_PARAMS)
    msg += bytes(32) + params
    assert len(msg) == 52 + 68 + 24 * DMIC_NUM_CTRL
    return msg


def dai_config(t, w, dai_type, dai_index):
    link = next((l for l in t.links if l.name == w.sname), None)
    if link is None:
        return None
    if dai_type == 2:
        hw = next((h for h in link.hw_configs if h.id == link.default_hw_config_id), link.hw_configs[0])
        return dmic_config(link, hw, dai_index, DMIC_MAX_PDMS)
    if dai_type != 1:      # SSP and DMIC only
        return None
    hw = next((h for h in link.hw_configs if h.id == link.default_hw_config_id), link.hw_configs[0])
    fmt = hw.fmt
    if hw.bclk_provider == 0:
        fmt |= SOF_DAI_FMT_CBP_CFP if hw.fsync_provider == 0 else SOF_DAI_FMT_CBP_CFC
    else:
        fmt |= SOF_DAI_FMT_CBC_CFP if hw.fsync_provider == 0 else SOF_DAI_FMT_CBC_CFC
    if hw.invert_bclk:
        fmt |= SOF_DAI_FMT_IB_IF if hw.invert_fsync else SOF_DAI_FMT_IB_NF
    else:
        fmt |= SOF_DAI_FMT_NB_IF if hw.invert_fsync else SOF_DAI_FMT_NB_NF
    mclk_id = tok_link(link, TKN_SSP_MCLK_ID, 0)
    ssp = struct.pack("<IHH", 0, 0, mclk_id)
    ssp += struct.pack("<III", hw.mclk_rate, hw.fsync_rate, hw.bclk_rate)
    ssp += struct.pack("<III", hw.tdm_slots, hw.rx_slots, hw.tx_slots)
    ssp += struct.pack("<IHH", tok_link(link, TKN_SSP_SAMPLE_BITS, 0), hw.tdm_slot_width, 0)
    ssp += struct.pack("<I", hw.mclk_direction)
    ssp += struct.pack("<HH", tok_link(link, TKN_SSP_FRAME_PULSE_WIDTH, 0), tok_link(link, TKN_SSP_TDM_PADDING, 0))
    ssp += struct.pack("<III", tok_link(link, TKN_SSP_CLKS_CONTROL, 0), tok_link(link, TKN_SSP_QUIRKS, 0),
                       tok_link(link, TKN_SSP_BCLK_DELAY, 0))
    assert len(ssp) == 60
    msg = struct.pack("<IIIIHBB", DAI_CONFIG_SIZE, DAI_MSG | DAI_CONFIG, 1, dai_index, fmt, 0, DAI_FLAGS_HW_PARAMS)
    msg += b"\0" * 32 + ssp
    msg += b"\0" * (DAI_CONFIG_SIZE - len(msg))
    return msg


def tok_link(link, token, default):
    for x in link.tuples:
        if x.token == token:
            return x.value
    return default


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tplg")
    ap.add_argument("out")
    ap.add_argument("--pipes", default="1")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--dmic-pdms", type=int, default=None, help="use only the first N PDM controllers (2 mics each)")
    a = ap.parse_args()
    global DMIC_MAX_PDMS
    DMIC_MAX_PDMS = a.dmic_pdms
    pipes = {int(x) for x in a.pipes.split(",")}
    t = Topology(a.tplg)
    msgs, pcms = build(t, pipes)

    blob = b"CBTP" + struct.pack("<III", 1, len(msgs), len(pcms))
    for pcm_id, direction, comp_id, pipe, name in pcms:
        blob += struct.pack("<IIII", pcm_id, direction, comp_id, pipe) + name.encode()[:31].ljust(32, b"\0")
    for _, m in msgs:
        assert len(m) % 4 == 0 and struct.unpack_from("<I", m)[0] == len(m), _
        blob += struct.pack("<I", len(m)) + m
    open(a.out, "wb").write(blob)

    if a.list:
        for name, m in msgs:
            size, cmd = struct.unpack_from("<II", m)
            print("%-34s cmd 0x%08X size %3d  %s" % (name, cmd, size, m[8:].hex()))
        for p in pcms:
            print("pcm", p)
    print("wrote %s: %d messages, %d pcms, %d bytes" % (a.out, len(msgs), len(pcms), len(blob)))


if __name__ == "__main__":
    main()
