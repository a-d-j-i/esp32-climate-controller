# Perfboard layout for the climate controller: parts on holes, point-to-point wires.
# Holes are (column, row), 1-based, seen from the component side.
#
#   python3 docs/layout.py          check the layout
#   python3 docs/layout.py --json   also write docs/layout.json (the wiring page's drawing data)
#
# The check fails if a net is not fully connected, if two wires on the same side of the
# board touch, if a wire passes over another net's solder joint, or if a top-side wire
# enters the antenna keep-out area. A "bridge" wire is an insulated jumper raised above the
# board: it may cross other wires but must not land on or pass over another net's joint.
# Nothing may be placed between a bottom connector and the board edge (the plugs go there).
import json, os, sys

COLS, ROWS = 24, 36
TOP = ["D23","D22","TX0","RX0","D21","D19","D18","D5","TX2","RX2","D4","D2","D15","GND","3V3"]
BOT = ["EN","VP","VN","D34","D35","D32","D33","D25","D26","D27","D14","D12","D13","GND","VIN"]
HEADER_NET = {"D23": "BOX", "TX2": "CAB", "RX2": "ROOM", "D4": "IRRX", "GND": "GND", "3V3": "3V3", "D27": "RELAY", "D32": "IRSIG", "VIN": "5V"}

parts = []   # {ref, kind, value, pads: [(c, r, net, label)]}
def part(ref, kind, value, pads, **extra):
    parts.append(dict(ref=ref, kind=kind, value=value, pads=pads, **extra))

# ESP32 female headers
part("H1", "header", "top row", [(6 + i, 12, HEADER_NET.get(n, "nc:" + n), n) for i, n in enumerate(TOP)])
part("H2", "header", "bottom row", [(6 + i, 22, HEADER_NET.get(n, "nc:" + n), n) for i, n in enumerate(BOT)])

# ---- top side: everything on 3.3 V
part("J1", "conn", "cabinet sensor", [(6, 2, "3V3", "3V3"), (7, 2, "CAB", "DATA"), (8, 2, "GND", "GND")], label="left")
part("R1", "res-stand", "10k", [(6, 3, "3V3", ""), (7, 3, "CAB", "")])
part("J2", "conn", "room sensor", [(10, 2, "3V3", "3V3"), (11, 2, "ROOM", "DATA"), (12, 2, "GND", "GND")])
part("R5", "res-stand", "10k", [(10, 3, "3V3", ""), (11, 3, "ROOM", "")])
# Box sensor (inside the controller's box, shown in status only): in the corner above the
# antenna keep-out; better on a short lead than plugged in here, next to the ESP32 module.
part("J6", "conn", "box sensor", [(2, 2, "3V3", "3V3"), (3, 2, "BOX", "DATA"), (4, 2, "GND", "GND")], label="left")
part("R10", "res-stand", "10k", [(2, 3, "3V3", ""), (3, 3, "BOX", "")], labelAt=(2.5, 4.2, "middle"))
part("U1", "irrx", "infrared receiver", [(24, 4, "IRRX", "OUT"), (24, 5, "GND", "GND"), (24, 6, "IRVCC", "VCC")])
part("C4", "cap", "10µ", [(23, 7, "GND", ""), (24, 7, "IRVCC", "")])
part("R6", "res", "100Ω", [(24, 8, "IRVCC", ""), (24, 12, "3V3", "")])
# ---- bottom side: everything on 5 V, every wire on the component side. The infrared LED
# driver sits under D32, the relay driver under D27, the relay header near VIN and the
# USB end. GND runs down column 19 and 5 V down column 20; the hole pairs (19, r)-(20, r)
# for r in 27..29 take extra supply capacitors. Two wires run left from the power
# columns and pass under the lying resistors instead of crossing anything: LED+ on row 24
# (from R4, under R7 and R2) and GND on row 26 (under R7 and R2). R4 and R9 lie across
# the GND column the same way. Both transistors go in E-B-C from the left.
#
# Relay driver: the module switches on when its input is low but its 5 V input
# transistor can't be switched fully off by 3.3 V, so Q2 pulls the module input low and
# R9 pulls it up to a clean 5 V. R8 holds the base low while the ESP32 starts. D27 HIGH =
# relay on (RELAY_ACTIVE_LOW false).
part("R2", "res", "1k", [(11, 23, "IRSIG", ""), (11, 27, "BASE", "")], bridges=[(11, 24), (11, 26)])
part("R3", "res-stand", "10k", [(11, 28, "BASE", ""), (10, 28, "GND", "")], labelAt=(9.4, 28, "end"))
part("Q1", "npn", "2N2222", [(10, 29, "GND", "E"), (11, 29, "BASE", "B"), (12, 29, "COLL", "C")], labelAt=(11, 30.8, "middle"))
part("J4", "conn5", "infrared LED", [(11, 33, "LEDP", "LED+"), (12, 33, "COLL", "LED−")], label="left")
part("R4", "res", "33Ω", [(16, 24, "LEDP", ""), (20, 24, "5V", "")], bridges=[(17, 24), (18, 24), (19, 24)])
part("R7", "res", "1k", [(15, 23, "RELAY", ""), (15, 27, "RBASE", "")], bridges=[(15, 24), (15, 26)])
part("R8", "res-stand", "10k", [(15, 28, "RBASE", ""), (14, 28, "GND", "")], labelAt=(16.4, 28, "start"))
part("Q2", "npn", "2N2222", [(14, 29, "GND", "E"), (15, 29, "RBASE", "B"), (16, 29, "RIN", "C")], labelAt=(15, 30.8, "middle"))
part("R9", "res", "10k", [(16, 31, "RIN", ""), (20, 31, "5V", "")], bridges=[(17, 31), (18, 31), (19, 31)])
part("J5", "conn5", "relay", [(18, 34, "RIN", "IN"), (19, 34, "GND", "GND"), (20, 34, "5V", "5V")])
part("C1", "elco", "470µ", [(20, 25, "5V", "+"), (19, 25, "GND", "−")])

wires = []   # {net, side: top|under, pts}
def wire(net, side, *pts):
    wires.append(dict(net=net, side=side, pts=list(pts)))

# ---- top side signals
wire("CAB", "top", (14, 12), (14, 8), (7, 8), (7, 3), (7, 2))
wire("ROOM", "top", (15, 12), (15, 6), (11, 6), (11, 3), (11, 2))
wire("BOX", "top", (6, 12), (6, 5), (3, 5), (3, 3), (3, 2))
wire("IRRX", "top", (16, 12), (16, 3), (24, 3), (24, 4))
wire("IRVCC", "top", (24, 6), (24, 7), (24, 8))
wire("GND", "top", (19, 9), (23, 9), (23, 7), (23, 5), (24, 5))
# ---- top side power, underneath
wire("3V3", "under", (20, 12), (20, 1), (10, 1), (6, 1), (6, 2), (6, 3))
wire("3V3", "under", (10, 1), (10, 2), (10, 3))
wire("3V3", "under", (6, 1), (2, 1), (2, 2), (2, 3))
wire("3V3", "under", (20, 12), (24, 12))
wire("GND", "under", (19, 12), (19, 9), (19, 4), (12, 4), (8, 4))
wire("GND", "under", (12, 4), (12, 2))
wire("GND", "under", (8, 4), (8, 2))
wire("GND", "under", (8, 4), (4, 4), (4, 2))
# ---- bottom side, all on top
wire("IRSIG", "top", (11, 22), (11, 23))
wire("BASE", "top", (11, 27), (11, 28), (11, 29))
wire("COLL", "top", (12, 29), (12, 33))
wire("LEDP", "top", (16, 24), (6, 24), (6, 32), (11, 32), (11, 33))
wire("RELAY", "top", (15, 22), (15, 23))
wire("RBASE", "top", (15, 27), (15, 28), (15, 29))
wire("RIN", "top", (16, 29), (16, 31), (16, 33), (18, 33), (18, 34))
wire("5V", "top", (20, 22), (20, 24), (20, 25), (20, 31), (20, 34))
wire("GND", "top", (19, 22), (19, 25), (19, 26), (19, 34))
wire("GND", "top", (19, 26), (14, 26), (10, 26))
wire("GND", "top", (10, 26), (10, 28), (10, 29))
wire("GND", "top", (14, 26), (14, 28), (14, 29))

KEEP_OUT = [(c, r) for c in range(1, 5) for r in range(9, 25)]

def holes_of(seg):
    (c1, r1), (c2, r2) = seg
    assert c1 == c2 or r1 == r2, ("not straight", seg)
    if c1 == c2:
        s = 1 if r2 >= r1 else -1
        return [(c1, r) for r in range(r1, r2 + s, s)]
    s = 1 if c2 >= c1 else -1
    return [(c, r1) for c in range(c1, c2 + s, s)]

def check():
    errors = []
    pad_net = {}
    for p in parts:
        for c, r, net, _ in p["pads"]:
            assert 1 <= c <= COLS and 1 <= r <= ROWS, (p["ref"], c, r)
            if (c, r) in pad_net: errors.append(f"two leads in hole {(c, r)}")
            pad_net[(c, r)] = net
    # solder points: pads and wire vertices
    joints = dict(pad_net)
    for w in wires:
        for v in w["pts"]:
            if v in joints and joints[v] != w["net"]:
                errors.append(f"{w['net']} wire joint at {v} which belongs to {joints[v]}")
            joints.setdefault(v, w["net"])
    # wires must not pass over a joint of another net; same-side wires must not touch
    used = {}  # (side, hole) -> net
    for w in wires:
        for a, b in zip(w["pts"], w["pts"][1:]):
            hs = holes_of((a, b))
            for h in hs[1:-1]:
                if h in joints and joints[h] != w["net"]:
                    errors.append(f"{w['net']} wire passes over {joints[h]} joint at {h}")
            for h in hs:
                if w["side"] == "bridge": continue
                k = (w["side"], h)
                if k in used and used[k] != w["net"]:
                    errors.append(f"{w['net']} and {used[k]} wires meet on the {w['side']} side at {h}")
                used[k] = w["net"]
                if w["side"] == "top" and h in KEEP_OUT:
                    errors.append(f"{w['net']} wire in the antenna keep-out at {h}")
    # the plugs of the bottom connectors need the holes between them and the edge
    for p in parts:
        if p["kind"] == "conn5":
            for c, r, *_ in p["pads"]:
                for rr in range(r + 1, ROWS + 1):
                    if ("top", (c, rr)) in used or ("under", (c, rr)) in used or (c, rr) in joints:
                        errors.append(f"{p['ref']}: hole {(c, rr)} between the connector and the edge is not free")
    # top wires under a lying part's body: only where the part says it bridges
    for p in parts:
        if p["kind"] != "res": continue
        (c1, r1, *_), (c2, r2, *_) = p["pads"]
        for h in holes_of(((c1, r1), (c2, r2)))[1:-1]:
            if ("top", h) in used and h not in p.get("bridges", []):
                errors.append(f"{used[('top', h)]} wire runs under {p['ref']} at {h}")
    # connectivity: every pad of a net reachable through that net's wires
    adj = {}
    def link(a, b):
        adj.setdefault(a, set()).add(b); adj.setdefault(b, set()).add(a)
    for w in wires:
        for a, b in zip(w["pts"], w["pts"][1:]):
            link(a, b)
    link((19, 12), (19, 22))  # the DevKit's two GND pins are joined on the DevKit itself
    nets = {}
    for h, n in pad_net.items():
        if not n.startswith("nc:"): nets.setdefault(n, []).append(h)
    for n, hs in nets.items():
        seen, todo = {hs[0]}, [hs[0]]
        while todo:
            x = todo.pop()
            for y in adj.get(x, ()):
                if y not in seen: seen.add(y); todo.append(y)
        missing = [h for h in hs if h not in seen]
        if missing: errors.append(f"net {n}: pads {missing} not connected to {hs[0]}")
    return errors, nets

errors, nets = check()
for e in errors: print("ERROR", e)
print("nets:", {n: len(h) for n, h in sorted(nets.items())})
print("OK" if not errors else f"{len(errors)} problems")
if "--json" in sys.argv:
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "layout.json")
    json.dump(dict(cols=COLS, rows=ROWS, parts=parts, wires=wires), open(out, "w"), ensure_ascii=False)
sys.exit(1 if errors else 0)
