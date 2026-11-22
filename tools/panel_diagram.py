#!/usr/bin/env python3
"""Draw the labelled SmplTrek panel diagram for the manual and the cheatsheet.

    tools/panel_diagram.py OUT.svg

The control positions are the measured control centres of the smpltrek-re emulator's panel
(emulator/ui/stock.js: 310 x 365 panel units, centres of each control), copied here as numbers so
the diagram builds without the emulator. Each button group gets a colour and a callout saying what
it does in STRATA; the words follow docs/CONTROLS.md. Standard library only; the output is plain
SVG, which GitHub renders inline.
"""
import sys

S = 2.0                     # SVG pixels per panel unit
PX, PY = 300, 40            # where the panel's top-left corner sits in the SVG
PW, PH = 310, 365           # panel size in panel units
W, H = PX + PW * S + 300, PY + PH * S + 40

# Group colours: distinct in hue and lightness so they still read in greyscale print.
C = {
    "screen":  "#9aa4b2",
    "knobs":   "#e0a030",
    "select":  "#c070e0",
    "arrows":  "#40a0f0",
    "func":    "#50c878",
    "okclr":   "#f06070",
    "steps":   "#f0d040",
    "upper":   "#ff8c40",
    "lower":   "#30c0c0",
    "octave":  "#e060a0",
}

# What each group does, after docs/CONTROLS.md. At most 40 characters a line.
CALLOUTS = {
    "screen": ("SCREEN", ["The mode, the track, and the four", "values the knobs A-D turn"]),
    "knobs": ("KNOBS A-D, VALUE", ["A-D: the four values on screen,", "push = that value's switch.",
                                   "VALUE: tempo; push + turn = swing"]),
    "select": ("SCREEN KEYS", ["The screen's pages: EDIT's six,", "MIXER's nine, PROJECT's five"]),
    "arrows": ("ARROW PAD", ["The cursor: UP/DOWN rows (tracks,", "lists), LEFT/RIGHT sideways"]),
    "func": ("FUNCTION BLOCK", ["Plain: go to that screen.", "FUNC + key: the word under it",
                                "(its printed second label)"]),
    "okclr": ("OK / CLR", ["OK picks or confirms, CLR goes back", "or clears. FUNC + CLR / OK:",
                           "undo / redo"]),
    "steps": ("STEP 1-16", ["The 16 steps of the selected track.", "Hold one and turn a knob = lock"]),
    "upper": ("PERFORMANCE PADS", ["Printed TRK 1-7; in STRATA the same", "seven tools in every mode.",
                                   "Lit dim, bright while held or on"]),
    "lower": ("TRACK PADS", ["Tracks 1-8: select and play.", "FUNC + pad: mute on the next bar"]),
    "octave": ("PAD-ROW KEYS", ["Which 16 the STEP keys mean: step", "page 1-4, KEYS octave, bank A-H.",
                                "FUNC + key: the track's length"]),
}

FUNC_KEYS = [("FUNC", 98, 174), ("REC", 135, 174), ("PROJECT", 172, 174), ("HOME", 209, 174),
             ("PLAY", 98, 200), ("STOP", 135, 200), ("MIXER", 172, 200), ("EDIT", 209, 200)]
# FUNC + key in STRATA: the key's printed second label (sampling, pool, system, <<, >>, fx, rec src).
SECOND = {"REC": "RESAMPLE", "PROJECT": "SAMPLES", "HOME": "MENU", "PLAY": "RESTART",
          "STOP": "RESET", "MIXER": "PERFORM", "EDIT": "MACHINE", "FUNC": "(hold: help)"}
PERFORM = ["FILL", "REPEAT", "MUTE", "SCENE A", "SCENE B", "THROW", "MUTATE"]
KNOBS = [("A", 214, 98, 13), ("B", 245, 98, 13), ("C", 214, 137, 13), ("D", 245, 137, 13),
         ("VALUE", 286, 139, 15)]
ARROWS = [("UP", 50, 169), ("LEFT", 31, 187), ("RIGHT", 69, 187), ("DOWN", 50, 205)]
UPPER_PRINT = ["TRK 1", "TRK 2", "TRK 3", "TRK 4", "TRK 5", "TRK 6", "TRK 7"]
LOWER_PRINT = ["TRK 8", "TRK 9", "TRK 10", "GLOBAL 1", "GLOBAL 2", "GLOBAL 3", "USB AUDIO", "EXT SRC"]
SCREEN = (113.5, 59, 76, 76)            # x, y, w, h: stock.css .screen-well


def p(x, y):
    return PX + x * S, PY + y * S


class Svg:
    def __init__(self):
        self.out = []

    def add(self, s):
        self.out.append(s)

    def rect(self, x, y, w, h, fill, stroke="none", rx=2, sw=1.5, extra=""):
        X, Y = p(x, y)
        self.add('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%.1f" fill="%s" '
                 'stroke="%s" stroke-width="%.1f"%s/>' % (X, Y, w * S, h * S, rx * S, fill, stroke, sw, extra))

    def circle(self, x, y, r, fill, stroke="none", sw=1.5):
        X, Y = p(x, y)
        self.add('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s" stroke="%s" stroke-width="%.1f"/>'
                 % (X, Y, r * S, fill, stroke, sw))

    def text(self, X, Y, s, size=11, fill="#e8ecf0", anchor="middle", weight="normal"):
        s = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        self.add('<text x="%.1f" y="%.1f" font-size="%.1f" fill="%s" text-anchor="%s" '
                 'font-weight="%s">%s</text>' % (X, Y, size, fill, anchor, weight, s))

    def ptext(self, x, y, s, size=5.0, fill="#e8ecf0", weight="normal"):
        X, Y = p(x, y)
        self.text(X, Y, s, size * S, fill, "middle", weight)


def centred_box(svg, x, y, w, h, colour, rx=2.5):
    svg.rect(x - w / 2, y - h / 2, w, h, "#2a2f36", colour, rx, 2.2)


def draw(svg):
    svg.add('<rect width="%d" height="%d" fill="#ffffff"/>' % (W, H))
    svg.rect(0, 0, PW, PH, "#1b1e23", "#3a3f46", 8, 2)

    # What STRATA does not use, in grey for orientation: jacks, speaker, input gains, VOL.
    for name, jx in (("IN", 27), ("SYNC", 53), ("OUT", 79)):
        svg.ptext(jx, 43, name, 3.6, "#6a737e")
        svg.circle(jx, 51, 4.5, "#0f1114", "#4a5058")
    for sy in (72, 82, 92):
        svg.rect(53 - 31, sy - 3.25, 62, 6.5, "#0f1114", "none", 3.25)
    for name, gx in (("L", 34), ("R", 80)):
        svg.ptext(gx, 118, name, 3.6, "#6a737e")
        svg.circle(gx, 131, 13, "#22262b", "#4a5058")
    svg.circle(262, 47, 11, "#22262b", "#4a5058")
    svg.ptext(285, 82, "VOL", 3.6, "#6a737e")
    svg.circle(286, 98, 15, "#22262b", "#4a5058")

    # The screen and the pair of select buttons under it.
    x, y, w, h = SCREEN
    svg.rect(x - 3, y - 3, w + 6, h + 6, "#050707", C["screen"], 3, 2.5)
    svg.ptext(x + w / 2, y + h / 2 + 2, "128 x 128", 5, "#6a737e")
    for sx in (132, 166):
        centred_box(svg, sx, 153, 22, 7, C["select"], 3.5)

    # Knobs A-D and VALUE.
    for name, kx, ky, r in KNOBS:
        svg.circle(kx, ky, r, "#2a2f36", C["knobs"], 2.4)
        svg.ptext(kx, ky + 2, name if name != "VALUE" else "VAL", 5.5, "#ffffff", "bold")

    # The arrow pad.
    svg.circle(50, 187, 31, "#15181c", C["arrows"], 2.4)
    faces = {"UP": "^", "DOWN": "v", "LEFT": "<", "RIGHT": ">"}
    for name, ax, ay in ARROWS:
        centred_box(svg, ax, ay, 17, 14, C["arrows"])
        svg.ptext(ax, ay + 2, faces[name], 6, "#ffffff", "bold")

    # The function block, with the printed second labels under each key.
    svg.rect(98 - 18 - 4, 174 - 10, 209 - 98 + 36 + 8, 200 - 174 + 26, "none", C["func"], 3, 2.4)
    for name, fx, fy in FUNC_KEYS:
        centred_box(svg, fx, fy, 30, 13, C["func"])
        svg.ptext(fx, fy + 1.8, name, 4.6, "#ffffff", "bold")
        if SECOND[name]:
            svg.ptext(fx, fy + 11, SECOND[name], 4.0, C["func"], "bold")

    # OK and CLR.
    for name, cx, cy in (("CLR", 248, 192), ("OK", 278, 178)):
        svg.circle(cx, cy, 10.5, "#2a2f36", C["okclr"], 2.4)
        svg.ptext(cx, cy + 1.8, name, 5, "#ffffff", "bold")

    # STEP 1-16.
    for i in range(16):
        sx, sy = 30 + 35.6 * (i % 8), 231 if i < 8 else 262
        centred_box(svg, sx, sy, 23, 23, C["steps"], 3)
        svg.ptext(sx, sy + 2, str(i + 1), 6, "#ffffff", "bold")

    # Pads: seven above (printed TRK 1-7), eight below, the octave pair at the upper row's ends.
    for side_x in (17, 293):
        centred_box(svg, side_x, 299, 13, 15, C["octave"], 3)
        svg.ptext(side_x, 301, "<" if side_x < 100 else ">", 6, "#ffffff", "bold")
    for i in range(15):
        upper = i % 2 == 1
        bx, by = (46 if upper else 28) + 35.6 * (i >> 1), 299 if upper else 339
        colour = C["upper"] if upper else C["lower"]
        centred_box(svg, bx, by, 27, 26, colour, 3)
        printed = UPPER_PRINT[i >> 1] if upper else LOWER_PRINT[i >> 1]
        svg.ptext(bx, by - 15, printed, 3.4, "#9aa4b2")
        if upper:
            svg.ptext(bx, by + 1.8, PERFORM[i >> 1], 4.3 if len(PERFORM[i >> 1]) > 5 else 5, "#ffffff", "bold")
        else:
            svg.ptext(bx, by + 2.5, "T%d" % ((i >> 1) + 1), 7, "#ffffff", "bold")


# Callouts: (group, side, label y in SVG pixels, anchor point on the panel in panel units).
ANCHORS = [
    ("screen", "L", 110, (113.5, 80)),
    ("select", "L", 230, (121, 153)),
    ("arrows", "L", 345, (19, 187)),
    ("steps", "L", 470, (19, 231)),
    ("octave", "L", 585, (11, 299)),
    ("lower", "L", 710, (15, 339)),
    ("knobs", "R", 250, (301, 120)),
    ("okclr", "R", 380, (288.5, 178)),
    ("func", "R", 470, (231, 213)),
    ("upper", "R", 590, (291, 284)),
]


def callouts(svg):
    for group, side, ly, (ax, ay) in ANCHORS:
        title, lines = CALLOUTS[group]
        colour = C[group]
        AX, AY = p(ax, ay)
        if side == "L":
            tx, anchor, lx = 16, "start", PX - 14
        else:
            tx, anchor, lx = W - 16, "end", PX + PW * S + 14
        svg.add('<polyline points="%.1f,%.1f %.1f,%.1f %.1f,%.1f" fill="none" stroke="%s" '
                'stroke-width="2"/>' % (lx, ly - 4, lx, ly - 4, AX, AY, colour))
        svg.add('<circle cx="%.1f" cy="%.1f" r="3.5" fill="%s"/>' % (AX, AY, colour))
        svg.text(tx, ly, title, 13, colour, anchor, "bold")
        for i, line in enumerate(l for l in lines if l):
            svg.text(tx, ly + 17 + 15 * i, line, 12, "#2a2f36", anchor)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    svg = Svg()
    svg.add('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
            'font-family="Helvetica, Arial, sans-serif">' % (W, H, W, H))
    draw(svg)
    callouts(svg)
    svg.add("</svg>")
    with open(sys.argv[1], "w") as f:
        f.write("\n".join(svg.out) + "\n")
    print("wrote %s" % sys.argv[1])


if __name__ == "__main__":
    main()
