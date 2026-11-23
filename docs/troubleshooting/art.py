# LiDE50 Scan - draws the illustrations of the troubleshooting guide as SVG, in the
# app's "liquid glass" look: pastel background, frosted cards, blue-violet accent.
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the LiDE50 Scan contributors
#
#   python3 docs/troubleshooting/art.py docs/troubleshooting/src
# then render each SVG with:  rsvg-convert -z 1.5 -o name.png name.svg
import os
import sys

ACCENT, ACCENT2 = "#1f7aff", "#6a5cf2"
TEXT, TEXT2, TEXT3 = "#1c1f2a", "#5b6071", "#9aa0ae"
OK, BAD, WARN = "#22a560", "#e5484d", "#f29d0c"
FONT = "Inter, 'Segoe UI', Arial, sans-serif"


def doc(w, h, body):
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">
<defs>
  <linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="#c3dcf8"/><stop offset="0.55" stop-color="#dcd3f4"/><stop offset="1" stop-color="#f5cfe3"/>
  </linearGradient>
  <radialGradient id="blob1" cx="0.15" cy="0.2" r="0.6"><stop offset="0" stop-color="#a9e4dc" stop-opacity="0.7"/><stop offset="1" stop-color="#a9e4dc" stop-opacity="0"/></radialGradient>
  <radialGradient id="blob2" cx="0.9" cy="0.85" r="0.55"><stop offset="0" stop-color="#f7b9d6" stop-opacity="0.6"/><stop offset="1" stop-color="#f7b9d6" stop-opacity="0"/></radialGradient>
  <linearGradient id="accent" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="{ACCENT}"/><stop offset="1" stop-color="{ACCENT2}"/></linearGradient>
  <linearGradient id="glass" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff" stop-opacity="0.86"/><stop offset="1" stop-color="#ffffff" stop-opacity="0.62"/></linearGradient>
  <linearGradient id="metal" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f4f5f8"/><stop offset="1" stop-color="#c9ccd6"/></linearGradient>
  <linearGradient id="dark" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#4a4f5e"/><stop offset="1" stop-color="#2b2f3a"/></linearGradient>
  <linearGradient id="beam" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff6c8" stop-opacity="0.95"/><stop offset="1" stop-color="#fff6c8" stop-opacity="0"/></linearGradient>
  <filter id="shadow" x="-20%" y="-20%" width="140%" height="150%"><feDropShadow dx="0" dy="8" stdDeviation="10" flood-color="#3b3f6b" flood-opacity="0.18"/></filter>
  <filter id="soft" x="-20%" y="-20%" width="140%" height="140%"><feDropShadow dx="0" dy="3" stdDeviation="4" flood-color="#3b3f6b" flood-opacity="0.16"/></filter>
</defs>
<rect width="{w}" height="{h}" rx="28" fill="url(#bg)"/>
<rect width="{w}" height="{h}" rx="28" fill="url(#blob1)"/>
<rect width="{w}" height="{h}" rx="28" fill="url(#blob2)"/>
{body}
</svg>
'''


def card(x, y, w, h, r=24):
    return (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="url(#glass)" filter="url(#shadow)"/>'
            f'<rect x="{x + 0.5}" y="{y + 0.5}" width="{w - 1}" height="{h - 1}" rx="{r}" fill="none" stroke="#ffffff" stroke-opacity="0.9"/>')


def text(x, y, s, size=17, weight=500, fill=TEXT, anchor="start"):
    s = s.replace("&", "&amp;").replace("<", "&lt;")
    return (f'<text x="{x}" y="{y}" font-family="{FONT}" font-size="{size}" font-weight="{weight}" '
            f'fill="{fill}" text-anchor="{anchor}">{s}</text>')


def badge(x, y, good):
    """A round tick or cross, as the app draws status dots."""
    col = OK if good else BAD
    mark = (f'<path d="M{x - 7} {y} l5 5 l9 -10" fill="none" stroke="#fff" stroke-width="3.2" stroke-linecap="round" stroke-linejoin="round"/>'
            if good else
            f'<path d="M{x - 6} {y - 6} l12 12 M{x + 6} {y - 6} l-12 12" stroke="#fff" stroke-width="3.2" stroke-linecap="round"/>')
    return f'<circle cx="{x}" cy="{y}" r="15" fill="{col}" filter="url(#soft)"/>' + mark


def title_row(x, y, good, s):
    return badge(x + 15, y - 6, good) + text(x + 42, y, s, 19, 650)


def pill(x, y, w, h, s, primary=False, size=15):
    fill = 'url(#accent)' if primary else 'url(#glass)'
    col = '#ffffff' if primary else TEXT
    return (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{h / 2}" fill="{fill}" filter="url(#soft)"/>'
            f'<rect x="{x + 0.5}" y="{y + 0.5}" width="{w - 1}" height="{h - 1}" rx="{h / 2}" fill="none" stroke="#ffffff" stroke-opacity="{0.35 if primary else 0.95}"/>'
            + text(x + w / 2, y + h / 2 + size * 0.36, s, size, 600, col, "middle"))


def arrow(x1, y1, x2, y2, col=TEXT3, w=3):
    return (f'<path d="M{x1} {y1} L{x2} {y2}" stroke="{col}" stroke-width="{w}" stroke-linecap="round"/>'
            f'<path d="M{x2 - 9} {y2 - 8} L{x2} {y2} L{x2 - 9} {y2 + 8}" fill="none" stroke="{col}" stroke-width="{w}" stroke-linecap="round" stroke-linejoin="round"/>')


# ------------------------------------------------------------------ objects

def scanner_top(x, y, w=210, h=150, label=True):
    """The LiDE 50 seen from above at an angle: a slim flatbed with a lid."""
    s = (f'<rect x="{x}" y="{y + 10}" width="{w}" height="{h}" rx="16" fill="url(#dark)" filter="url(#shadow)"/>'
         f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="16" fill="url(#metal)"/>'
         f'<rect x="{x + 0.5}" y="{y + 0.5}" width="{w - 1}" height="{h - 1}" rx="16" fill="none" stroke="#ffffff"/>'
         f'<rect x="{x + 14}" y="{y + 14}" width="{w - 28}" height="{h - 42}" rx="8" fill="#e6e8ef" stroke="#cfd3dc"/>'
         f'<circle cx="{x + w - 26}" cy="{y + h - 15}" r="5" fill="{OK}"/>')
    if label:
        s += text(x + 18, y + h - 10, "CanoScan LiDE 50", 11, 600, TEXT2)
    return s


def computer(x, y):
    """A desktop tower with ports at the back."""
    return (f'<rect x="{x}" y="{y}" width="110" height="190" rx="14" fill="url(#metal)" filter="url(#shadow)"/>'
            f'<rect x="{x + 0.5}" y="{y + 0.5}" width="109" height="189" rx="14" fill="none" stroke="#fff"/>'
            f'<rect x="{x + 16}" y="{y + 20}" width="78" height="8" rx="4" fill="#c4c8d3"/>'
            f'<rect x="{x + 16}" y="{y + 34}" width="78" height="8" rx="4" fill="#c4c8d3"/>'
            + "".join(f'<rect x="{x + 30}" y="{y + 112 + i * 18}" width="26" height="10" rx="2" fill="#2f3340"/>' for i in range(3))
            + text(x + 55, y + 182, "back of the PC", 11, 600, TEXT2, "middle"))


def usb_hub(x, y):
    return (f'<rect x="{x}" y="{y}" width="120" height="44" rx="12" fill="url(#metal)" filter="url(#soft)"/>'
            + "".join(f'<rect x="{x + 14 + i * 26}" y="{y + 17}" width="18" height="9" rx="2" fill="#2f3340"/>' for i in range(4))
            + text(x + 60, y + 64, "hub, keyboard or monitor", 11, 600, TEXT2, "middle"))


def cable(points, col="#3b4050"):
    d = "M" + " ".join(f"{px} {py}" for px, py in points)
    return f'<path d="{d}" fill="none" stroke="{col}" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/>'


# ------------------------------------------------------------- illustrations

def usb_direct():
    W, H = 1000, 420
    b = card(24, 24, 466, 372) + card(510, 24, 466, 372)
    # left: straight into the PC
    b += title_row(48, 70, True, "Straight into the computer")
    b += scanner_top(60, 150)
    b += cable([(270, 215), (318, 215), (318, 300), (360, 300)])
    b += computer(360, 140)
    b += text(48, 375, "On a desktop, use a port at the back.", 14, 500, TEXT2)
    # right: through a hub
    b += title_row(534, 70, False, "Through a hub or keyboard")
    b += scanner_top(546, 150)
    b += cable([(756, 215), (800, 215), (800, 248), (830, 248)])
    b += usb_hub(830, 226)
    b += text(534, 360, "The LiDE 50 has no power adapter: it runs on", 14, 500, TEXT2)
    b += text(534, 380, "USB power, and many hubs can't give it enough.", 14, 500, TEXT2)
    return doc(W, H, b)


def side_view(x, y, lid_open, book=False):
    """The scanner from the side: base, glass, head, white strip and lid."""
    s = (f'<rect x="{x}" y="{y + 40}" width="330" height="34" rx="10" fill="url(#metal)" filter="url(#soft)"/>'
         f'<rect x="{x + 12}" y="{y + 36}" width="306" height="6" rx="3" fill="#bfe3f4" opacity="0.9"/>'
         # white calibration strip at the left end, under the glass
         f'<rect x="{x + 16}" y="{y + 44}" width="26" height="6" rx="2" fill="#ffffff" stroke="#c9ccd6"/>'
         # scan head
         f'<rect x="{x + 60}" y="{y + 48}" width="22" height="20" rx="4" fill="url(#accent)"/>')
    if lid_open:
        s += f'<g transform="rotate(-24 {x + 6} {y + 34})"><rect x="{x}" y="{y + 18}" width="330" height="16" rx="8" fill="url(#metal)" filter="url(#soft)"/></g>'
        # room light pouring in
        s += f'<path d="M{x + 140} {y - 82} L{x + 330} {y - 82} L{x + 318} {y + 36} L{x + 60} {y + 36} Z" fill="url(#beam)"/>'
        for i, lx in enumerate((170, 225, 280)):
            s += f'<path d="M{x + lx} {y - 70} L{x + lx - 26} {y + 20}" stroke="{WARN}" stroke-width="3" stroke-linecap="round" stroke-dasharray="2 9"/>'
        if book:
            s += f'<rect x="{x + 90}" y="{y + 2}" width="200" height="34" rx="6" fill="#d9c7a7" stroke="#b59d74"/>'
            s += text(x + 190, y + 25, "thick book", 12, 600, "#6b5636", "middle")
    else:
        s += f'<rect x="{x}" y="{y + 18}" width="330" height="18" rx="9" fill="url(#metal)" filter="url(#soft)"/>'
        s += f'<rect x="{x + 0.5}" y="{y + 18.5}" width="329" height="17" rx="9" fill="none" stroke="#fff"/>'
    return s


def lid_light():
    W, H = 1000, 380
    b = card(24, 24, 466, 332) + card(510, 24, 466, 332)
    b += title_row(48, 70, True, "Lid closed, flat")
    b += side_view(92, 160, False)
    b += text(92, 262, "white strip", 12, 600, TEXT2)
    b += f'<path d="M118 250 L120 210" stroke="{TEXT3}" stroke-width="2"/>'
    b += text(48, 300, "The scanner measures its light on a white strip", 14, 500, TEXT2)
    b += text(48, 320, "at the end where the head rests.", 14, 500, TEXT2)
    b += title_row(534, 70, False, "Lid open, or a thick book")
    b += side_view(580, 190, True, book=True)
    b += text(534, 300, "Room light reaches the sensor and upsets the", 14, 500, TEXT2)
    b += text(534, 320, "calibration. Press the lid down or dim the room.", 14, 500, TEXT2)
    return doc(W, H, b)


def lock_switch():
    W, H = 1000, 360
    b = card(24, 24, 952, 312)
    b += text(52, 72, "Underside of the scanner", 19, 650)
    b += text(52, 98, "If your LiDE 50 has a lock switch, it must be in the unlocked position before scanning.", 14, 500, TEXT2)
    # the scanner upside down
    b += f'<rect x="110" y="140" width="420" height="160" rx="20" fill="url(#dark)" filter="url(#shadow)"/>'
    b += f'<rect x="130" y="160" width="380" height="120" rx="12" fill="none" stroke="#5b6170" stroke-dasharray="4 6"/>'
    for fx, fy in ((140, 150), (500, 150), (140, 290), (500, 290)):
        b += f'<circle cx="{fx}" cy="{fy - 2 if fy < 200 else fy - 18}" r="7" fill="#1e2129"/>'
    # switch
    b += f'<rect x="270" y="205" width="120" height="34" rx="17" fill="#1e2129" stroke="#666c7c"/>'
    b += f'<rect x="336" y="209" width="50" height="26" rx="13" fill="url(#accent)"/>'
    b += arrow(600, 222, 660, 222, ACCENT)
    # zoomed switch
    b += card(690, 140, 250, 160, 22)
    b += f'<rect x="720" y="196" width="190" height="48" rx="24" fill="#2b2f3a"/>'
    b += f'<rect x="818" y="200" width="88" height="40" rx="20" fill="url(#accent)"/>'
    b += text(762, 227, "Locked", 13, 600, "#9aa0ae", "middle")
    b += text(862, 226, "Unlocked", 13, 700, "#ffffff", "middle")
    b += badge(815, 280, True)
    b += text(52, 322, "Slide it only with the scanner unplugged, and lock it again before carrying the scanner.", 14, 500, TEXT2)
    return doc(W, H, b)


def smartscreen():
    W, H = 1000, 250
    b = card(24, 24, 952, 202)
    b += text(52, 70, "The first time you start LiDE50Scan.exe", 19, 650)
    b += text(52, 96, "Windows doesn't know the app yet, so it may stop it once. That's expected for a new, unsigned program.", 14, 500, TEXT2)
    steps = [("1", "“Windows protected your PC”"), ("2", "Click  More info"), ("3", "Click  Run anyway")]
    x = 52
    for i, (n, s) in enumerate(steps):
        w = 268 if i == 0 else 230
        b += pill(x, 132, w, 52, s, primary=(i == 2), size=15)
        if i < 2:
            b += arrow(x + w + 14, 158, x + w + 60, 158, TEXT3)
        x += w + 76
    return doc(W, H, b)


def files():
    W, H = 1000, 330
    b = card(24, 24, 952, 282)
    b += text(52, 70, "Next to LiDE50Scan.exe", 19, 650)
    b += text(52, 96, "Everything the app keeps is in its own folder. Nothing is installed anywhere else.", 14, 500, TEXT2)
    rows = [("folder", "LiDE50Scan-data", "settings and the calibration cache", False),
            ("app", "LiDE50Scan.exe", "the app", False),
            ("log", "LiDE50Scan-log.txt", "the activity log: attach it to a bug report", True),
            ("doc", "Troubleshooting.html", "this guide", False),
            ("doc", "README.txt", "the manual", False)]
    y = 122
    for kind, name, what, hi in rows:
        if hi:
            b += f'<rect x="44" y="{y - 4}" width="912" height="34" rx="12" fill="url(#accent)" opacity="0.14"/>'
            b += f'<rect x="44.5" y="{y - 3.5}" width="911" height="33" rx="12" fill="none" stroke="{ACCENT}" stroke-opacity="0.55"/>'
        ix = 62
        if kind == "folder":
            b += f'<path d="M{ix} {y + 6} h10 l4 4 h14 v14 h-28 z" fill="#f2b84b"/>'
        elif kind == "app":
            b += f'<rect x="{ix}" y="{y + 3}" width="26" height="22" rx="6" fill="url(#accent)"/><rect x="{ix + 7}" y="{y + 9}" width="12" height="10" rx="2" fill="none" stroke="#fff" stroke-width="2"/>'
        else:
            b += f'<path d="M{ix + 4} {y + 2} h13 l7 7 v18 h-20 z" fill="#ffffff" stroke="{ACCENT if kind == "log" else TEXT3}" stroke-width="1.5"/>'
            b += "".join(f'<path d="M{ix + 8} {y + 13 + i * 4} h12" stroke="{TEXT3}" stroke-width="1.5"/>' for i in range(3))
        b += text(104, y + 19, name, 15, 700 if hi else 600, ACCENT if hi else TEXT)
        b += text(330, y + 19, what, 14, 500, TEXT2)
        y += 34
    return doc(W, H, b)


def device_manager():
    W, H = 1000, 330
    b = card(24, 24, 466, 282) + card(510, 24, 466, 282)
    b += text(48, 70, "Device Manager, ready for LiDE50 Scan", 17, 650)

    def node(x, y, s, open_=True, child=False, hi=False, col=None):
        out = ""
        if not child:
            out += f'<path d="M{x} {y - 9} l6 5 l-6 5" fill="none" stroke="{TEXT2}" stroke-width="2" transform="rotate({90 if open_ else 0} {x + 3} {y - 4})"/>'
        if hi:
            out += f'<rect x="{x + 10}" y="{y - 22}" width="{len(s) * 8.2 + 44}" height="32" rx="10" fill="url(#accent)" opacity="0.14"/>'
        out += f'<rect x="{x + 18}" y="{y - 14}" width="16" height="14" rx="3" fill="{col or TEXT3}"/>'
        out += text(x + 42, y, s, 14, 600 if hi else 500, TEXT)
        return out
    b += node(56, 120, "Imaging devices", open_=False)
    b += node(56, 160, "Universal Serial Bus controllers", open_=False)
    b += node(56, 200, "Universal Serial Bus devices")
    b += node(86, 238, "WinUsb Device  (the LiDE 50)", child=True, hi=True, col=ACCENT)
    b += text(48, 286, "WinUSB is Windows' own driver; LiDE50 Scan talks through it.", 13, 500, TEXT2)

    b += text(534, 70, "Things that stop it", 17, 650)
    b += badge(549, 112, False) + text(574, 117, "A down arrow on the icon: the device is", 14, 600)
    b += text(574, 137, "disabled. Right-click it, Enable device.", 14, 500, TEXT2)
    b += badge(549, 180, False) + text(574, 185, "Listed under Imaging devices: Canon's", 14, 600)
    b += text(574, 205, "driver has it. Use Enable scanner access.", 14, 500, TEXT2)
    b += badge(549, 248, False) + text(574, 253, "A yellow ! (code 43 or 10): unplug it, use", 14, 600)
    b += text(574, 273, "another port, check the cable.", 14, 500, TEXT2)
    return doc(W, H, b)


def _thumbs(paths):
    import base64
    out = []
    for p in paths:
        with open(p, "rb") as f:
            out.append("data:image/png;base64," + base64.b64encode(f.read()).decode())
    return out


def samples(title, sub, items, w_img=200, h_img=250):
    """A row of example scans, each with a tick/cross and a caption."""
    n = len(items)
    W = 52 * 2 + n * w_img + (n - 1) * 44
    H = 150 + h_img + 70
    b = card(24, 24, W - 48, H - 48)
    b += text(52, 72, title, 19, 650) + text(52, 98, sub, 14, 500, TEXT2)
    x = 52
    for href, good, cap, cap2 in items:
        b += f'<rect x="{x}" y="122" width="{w_img}" height="{h_img}" rx="14" fill="#fff" filter="url(#soft)"/>'
        b += f'<clipPath id="c{x}"><rect x="{x}" y="122" width="{w_img}" height="{h_img}" rx="14"/></clipPath>'
        b += f'<image href="{href}" x="{x}" y="122" width="{w_img}" height="{h_img}" preserveAspectRatio="xMidYMid slice" clip-path="url(#c{x})"/>'
        b += f'<rect x="{x + 0.5}" y="122.5" width="{w_img - 1}" height="{h_img - 1}" rx="14" fill="none" stroke="#ffffff"/>'
        b += badge(x + w_img - 20, 142, good)
        b += text(x, 122 + h_img + 28, cap, 15, 650)
        b += text(x, 122 + h_img + 48, cap2, 13, 500, TEXT2)
        x += w_img + 44
    return doc(W, H, b)


def colours(src):
    t = _thumbs([os.path.join(src, n) for n in ("sample-normal.png", "sample-green.png", "sample-magenta.png")])
    return samples("What colour problems look like",
                   "Three scans of the same page. Colour casts come from the lamp, not from the page.",
                   [(t[0], True, "Normal", "white paper looks white"),
                    (t[1], False, "Green cast", "one LED weak (fixed in 1.45)"),
                    (t[2], False, "One colour missing", "an LED gave no light")])


def stretched(src):
    t = _thumbs([os.path.join(src, n) for n in ("sample-part.png", "sample-stretched.png")])
    return samples("A stretched scan",
                   "Circles turn into ovals, often with a grinding noise from the scanner.",
                   [(t[0], True, "Normal", "the circle is round"),
                    (t[1], False, "Stretched", "the motor skipped steps")], 300, 250)


ART = {
    "usb-direct": usb_direct,
    "lid-light": lid_light,
    "lock-switch": lock_switch,
    "smartscreen": smartscreen,
    "files": files,
    "device-manager": device_manager,
}

SAMPLE_ART = {"colours": colours, "stretched": stretched}

if __name__ == "__main__":
    # art.py <out dir> [<folder with sample-*.png>]
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    for name, fn in ART.items():
        with open(os.path.join(out, name + ".svg"), "w", encoding="utf-8") as f:
            f.write(fn())
        print(name)
    if len(sys.argv) > 2:
        for name, fn in SAMPLE_ART.items():
            with open(os.path.join(out, name + ".svg"), "w", encoding="utf-8") as f:
                f.write(fn(sys.argv[2]))
            print(name)
