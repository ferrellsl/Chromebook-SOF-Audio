"""ALSA SoC topology (.tplg) reader, enough for SOF IPC3 topologies.

Layout follows include/uapi/sound/asoc.h (Linux, MIT-style UAPI header).
Usage:  python tools/tplg.py drivers/cbsof/firmware/sof-glk-da7219.tplg
"""
import re
import struct
import sys
from dataclasses import dataclass, field

NAME = 44
TPLG_MAGIC = b"CoSA"
T_MIXER, T_BYTES, T_ENUM, T_GRAPH, T_WIDGET, T_DAI_LINK, T_PCM, T_MANIFEST, \
    T_CODEC_LINK, T_BACKEND_LINK, T_PDATA, T_DAI = range(1, 13)
TYPE_NAMES = {1: "MIXER", 2: "BYTES", 3: "ENUM", 4: "GRAPH", 5: "WIDGET", 6: "DAI_LINK",
              7: "PCM", 8: "MANIFEST", 9: "CODEC_LINK", 10: "BE_LINK", 11: "PDATA", 12: "DAI"}
WIDGET_NAMES = {0: "input", 1: "output", 2: "mux", 3: "mixer", 4: "pga", 5: "out_drv",
                6: "adc", 7: "dac", 8: "switch", 9: "pre", 10: "post", 11: "aif_in",
                12: "aif_out", 13: "dai_in", 14: "dai_out", 15: "dai_link", 16: "buffer",
                17: "scheduler", 18: "effect", 19: "siggen", 20: "src", 21: "asrc",
                22: "encoder", 23: "decoder"}
TUPLE_UUID, TUPLE_STRING, TUPLE_BOOL, TUPLE_BYTE, TUPLE_WORD, TUPLE_SHORT = range(6)

CTL_HDR_SIZE = 4 + 4 + NAME + 4 + 12 + (4 + 4 + 32 * 4)   # = 204


def cstr(b):
    return b.split(b"\0", 1)[0].decode("ascii", "replace")


def load_token_names(path):
    """Map token value -> name from Linux include/uapi/sound/sof/tokens.h."""
    names = {}
    try:
        for line in open(path, encoding="utf-8", errors="replace"):
            m = re.match(r"#define\s+(SOF_TKN_\w+)\s+(\d+)", line)
            if m:
                names[int(m.group(2))] = m.group(1)
    except OSError:
        pass
    return names


@dataclass
class Tuple:
    token: int
    type: int
    value: object


def parse_private(data):
    """Private data is a sequence of vendor arrays of tuples."""
    tuples = []
    p = 0
    while p + 12 <= len(data):
        size, typ, num = struct.unpack_from("<III", data, p)
        if size < 12 or p + size > len(data):
            break
        q = p + 12
        for _ in range(num):
            if typ == TUPLE_UUID:
                tok = struct.unpack_from("<I", data, q)[0]
                tuples.append(Tuple(tok, typ, data[q + 4:q + 20]))
                q += 20
            elif typ == TUPLE_STRING:
                tok = struct.unpack_from("<I", data, q)[0]
                tuples.append(Tuple(tok, typ, cstr(data[q + 4:q + 4 + NAME])))
                q += 4 + NAME
            else:
                tok, val = struct.unpack_from("<II", data, q)
                tuples.append(Tuple(tok, typ, val))
                q += 8
        p += size
    return tuples


@dataclass
class Control:
    kind: str          # mixer / enum / bytes
    name: str
    info: int
    raw: bytes
    tuples: list
    priv: bytes
    # mixer
    min: int = 0
    max: int = 0
    num_channels: int = 0
    tlv: tuple = ()
    # bytes
    bytes_max: int = 0


@dataclass
class Widget:
    index: int         # pipeline index of the block
    id: int
    name: str
    sname: str
    tuples: list
    priv: bytes
    controls: list = field(default_factory=list)

    @property
    def kind(self):
        return WIDGET_NAMES.get(self.id, str(self.id))


@dataclass
class Route:
    index: int
    sink: str
    control: str
    source: str


@dataclass
class Pcm:
    name: str
    dai_name: str
    pcm_id: int
    dai_id: int
    playback: int
    capture: int
    tuples: list


@dataclass
class HwConfig:
    id: int
    fmt: int
    clock_gated: int
    invert_bclk: int
    invert_fsync: int
    bclk_provider: int
    fsync_provider: int
    mclk_direction: int
    mclk_rate: int
    bclk_rate: int
    fsync_rate: int
    tdm_slots: int
    tdm_slot_width: int
    tx_slots: int
    rx_slots: int


@dataclass
class Link:
    id: int
    name: str
    stream_name: str
    hw_configs: list
    default_hw_config_id: int
    tuples: list
    priv: bytes


def parse_control(data, p):
    size_hdr, ctype = struct.unpack_from("<II", data, p)
    name = cstr(data[p + 8:p + 8 + NAME])
    access = struct.unpack_from("<I", data, p + 8 + NAME)[0]
    get, put, info = struct.unpack_from("<III", data, p + 12 + NAME)
    tlv_size, tlv_type = struct.unpack_from("<II", data, p + 24 + NAME)
    tlv = struct.unpack_from("<III", data, p + 32 + NAME)
    size = struct.unpack_from("<I", data, p + CTL_HDR_SIZE)[0]
    priv_size = struct.unpack_from("<I", data, p + size - 4)[0]
    priv = data[p + size:p + size + priv_size]
    if info in (1, 2, 3, 7, 8, 64, 68):
        kind = "mixer"
    elif info in (4, 6, 65, 66, 67):
        kind = "enum"
    elif info == 5:
        kind = "bytes"
    else:
        kind = "ctl%d" % info
    c = Control(kind, name, info, data[p:p + size], parse_private(priv), priv)
    if kind == "mixer":
        c.min, c.max, _pmax, _inv, c.num_channels = struct.unpack_from("<IIIII", data, p + CTL_HDR_SIZE + 4)
        c.tlv = tlv if tlv_size else ()
    elif kind == "bytes":
        c.bytes_max = struct.unpack_from("<I", data, p + CTL_HDR_SIZE + 4)[0]
    return c, size + priv_size


class Topology:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        self.widgets, self.routes, self.pcms, self.links, self.dais = [], [], [], [], []
        self.manifest_tuples = []
        self.blocks = []
        self._parse()

    def _parse(self):
        d = self.data
        off = 0
        while off + 36 <= len(d):
            magic, abi, ver, typ, hsize, vtype, psize, index, count = struct.unpack_from("<4sIIIIIIII", d, off)
            if magic != TPLG_MAGIC:
                raise ValueError("bad topology magic at 0x%x" % off)
            body = d[off + hsize:off + hsize + psize]
            self.blocks.append((TYPE_NAMES.get(typ, typ), index, count, psize))
            if typ == T_WIDGET:
                self._widgets(body, index, count)
            elif typ == T_GRAPH:
                for i in range(count):
                    b = body[i * 3 * NAME:(i + 1) * 3 * NAME]
                    self.routes.append(Route(index, cstr(b[:NAME]), cstr(b[NAME:2 * NAME]), cstr(b[2 * NAME:])))
            elif typ == T_PCM:
                self._pcms(body, count)
            elif typ in (T_DAI_LINK, T_BACKEND_LINK):
                self._links(body, count)
            elif typ == T_MANIFEST:
                size = struct.unpack_from("<I", body, 0)[0]
                psz = struct.unpack_from("<I", body, size - 4)[0]
                self.manifest_tuples = parse_private(body[size:size + psz])
            off += hsize + psize

    def _widgets(self, body, index, count):
        p = 0
        for _ in range(count):
            size, wid = struct.unpack_from("<II", body, p)
            name = cstr(body[p + 8:p + 8 + NAME])
            sname = cstr(body[p + 8 + NAME:p + 8 + 2 * NAME])
            nk = struct.unpack_from("<I", body, p + size - 8)[0]
            psz = struct.unpack_from("<I", body, p + size - 4)[0]
            priv = body[p + size:p + size + psz]
            w = Widget(index, wid, name, sname, parse_private(priv), priv)
            p += size + psz
            for _ in range(nk):
                c, n = parse_control(body, p)
                w.controls.append(c)
                p += n
            self.widgets.append(w)

    def _pcms(self, body, count):
        p = 0
        for _ in range(count):
            size = struct.unpack_from("<I", body, p)[0]
            name = cstr(body[p + 4:p + 4 + NAME])
            dai = cstr(body[p + 4 + NAME:p + 4 + 2 * NAME])
            pcm_id, dai_id, pb, cap = struct.unpack_from("<IIII", body, p + 4 + 2 * NAME)
            psz = struct.unpack_from("<I", body, p + size - 4)[0]
            self.pcms.append(Pcm(name, dai, pcm_id, dai_id, pb, cap, parse_private(body[p + size:p + size + psz])))
            p += size + psz

    def _links(self, body, count):
        stream_size = 4 + NAME + 8 + 4 * 4   # 72
        hw_size = 4 + 4 + 4 + 6 + 2 + 4 * 8 + 4 * 8 + 4 + 4 * 8
        p = 0
        for _ in range(count):
            size, lid = struct.unpack_from("<II", body, p)
            name = cstr(body[p + 8:p + 8 + NAME])
            sname = cstr(body[p + 8 + NAME:p + 8 + 2 * NAME])
            q = p + 8 + 2 * NAME + 8 * stream_size
            num_streams = struct.unpack_from("<I", body, q)[0]
            q += 4
            hws = []
            for i in range(8):
                h = q + i * hw_size
                hsz, hid, fmt = struct.unpack_from("<III", body, h)
                cg, ib, ifs, bp, fp, md = struct.unpack_from("<6B", body, h + 12)
                vals = struct.unpack_from("<7I", body, h + 20)
                hws.append(HwConfig(hid, fmt, cg, ib, ifs, bp, fp, md, *vals))
            q += 8 * hw_size
            num_hw, default_hw = struct.unpack_from("<II", body, q)
            psz = struct.unpack_from("<I", body, p + size - 4)[0]
            priv = body[p + size:p + size + psz]
            self.links.append(Link(lid, name, sname, hws[:num_hw], default_hw, parse_private(priv), priv))
            p += size + psz


def fmt_tuple(t, names):
    n = names.get(t.token, "tok%d" % t.token)
    if t.type == TUPLE_UUID:
        v = t.value.hex()
    else:
        v = t.value
    return "%s=%s" % (n, v)


def main():
    path = sys.argv[1]
    tok = load_token_names(sys.argv[2] if len(sys.argv) > 2 else
                           "reference/sof-linux/include/uapi/sound/sof/tokens.h")
    t = Topology(path)
    print("blocks:", t.blocks)
    print("manifest:", [fmt_tuple(x, tok) for x in t.manifest_tuples])
    for w in t.widgets:
        print("\n[pipe %d] %-8s %-28s sname=%r" % (w.index, w.kind, w.name, w.sname))
        for x in w.tuples:
            print("      ", fmt_tuple(x, tok))
        for c in w.controls:
            extra = " min=%d max=%d ch=%d tlv=%s" % (c.min, c.max, c.num_channels, c.tlv) if c.kind == "mixer" else \
                    " max=%d privlen=%d" % (c.bytes_max, len(c.priv)) if c.kind == "bytes" else ""
            print("       control %s %r%s" % (c.kind, c.name, extra), [fmt_tuple(x, tok) for x in c.tuples])
    print("\nroutes:")
    for r in t.routes:
        print("  [pipe %d] %s <- %s%s" % (r.index, r.sink, r.source, (" (%s)" % r.control) if r.control else ""))
    print("\npcms:")
    for p in t.pcms:
        print("  id %d %r dai %r dai_id %d playback %d capture %d" % (p.pcm_id, p.name, p.dai_name, p.dai_id, p.playback, p.capture),
              [fmt_tuple(x, tok) for x in p.tuples])
    print("\nlinks:")
    for l in t.links:
        print("  id %d %r stream %r default_hw %d" % (l.id, l.name, l.stream_name, l.default_hw_config_id))
        for h in l.hw_configs:
            print("     hw", h)
        for x in l.tuples:
            print("      ", fmt_tuple(x, tok))


if __name__ == "__main__":
    main()
