r"""Character-select art for extra classes, added to Win/s_o0a00.osb.

For an extra class (class 6+ sits on page class // 6, panel class % 6):
  - panel_art: its sepia panel picture, a copy of an exported panel picture
    (s_o0a00\mesh_m135..m140_o-24_24.png) redrawn; the file name keeps the position.
  - highlight_art: its coloured picture shown on the selected panel, a copy of an
    exported highlight (s_o0a00\a043_f00_m56_o-32_24.png = Empress, ...) redrawn.
Build mod adds them as new pictures, an animation MOD_SEL_<class> for the
highlight (a copy of the panel's own), and per page with panel art a copy of the
panel layout (キャラクターセレクト2) named MOD_SEL_PAGE<page>; panels with no class
on a page show their picture greyed (they can't be picked). The content mod
switches to these when the page changes.

The page prompt ("RT Page 1/2" under Confirm / Return) is drawn by the game itself:
its prompt layout (キャラセレクト_レイアウト2) gets a third icon part and text part,
and PadInfo.tb2 gets the texts MOD_PAGE_<n> (see build_mod); the content mod adds the
third prompt.
"""
import os
import re
import struct

import layout
import osb
import osbedit
import sprites

LAYOUT = "キャラクターセレクト2"
PROMPT_LAYOUT = "キャラセレクト_レイアウト2"
HIGHLIGHTS = ["皇女2", "戦士2", "忍者2", "魔女2", "巫女", "盗賊"]  # per panel, Empress .. Bandit
ROOT = 0x8D154D2D
PROMPT_STEP = -15.0  # the prompt lines are 15 units apart
# Empty panels (no class there on that page; they can't be picked) and classes with no
# panel picture: the panel's outline filled dark, with the "empty" dash (see below).
EMPTY_PANEL = (108, 68, 40, 255)  # the dark brown of the select screen's wood


def image_tiles(w, h, img, ox, oy):
    """16x16 canvas tiles (BGRA) of a full-colour picture whose top-left canvas point is (ox, oy)."""
    T = sprites.TILE
    blocks = {}
    for row in range(h):
        for col in range(w):
            p = img[(row * w + col) * 4:(row * w + col) * 4 + 4]
            if p[3]:
                blocks.setdefault((col // T, row // T), []).append((col, row, p))
    tiles = []
    for (bc, br), pixels in sorted(blocks.items()):
        tile = bytearray(T * T * 4)
        for col, row, p in pixels:
            t = ((row - br * T) * T + (col - bc * T)) * 4
            tile[t:t + 4] = bytes((p[2], p[1], p[0], p[3]))
        tiles.append((ox + bc * T, oy - br * T - T, bytes(tile)))
    return tiles


def png_with_origin(path):
    m = re.search(sprites.FRAME_RE, os.path.basename(path))
    if not m:
        raise ValueError("%s: keep the exported file name (it ends like _m135_o-24_24.png)" % os.path.basename(path))
    w, h, img = osb.read_png_rgba(path)
    return w, h, img, int(m.group(2)), int(m.group(3))


def panel_mesh(m, panel):
    """The picture the select layout shows on panel 0..5."""
    a = m["anims"][osbedit.find_anim(m, layout.ice(LAYOUT))]
    for blk in m["blocks"][a["first"]:a["first"] + a["count"]]:
        if struct.unpack_from("<I", blk["block"], 4)[0] == layout.ice("PL%d" % (panel + 1)):
            return osbedit.track_value(blk["tracks"], 0)
    raise ValueError("the select layout has no panel PL%d" % (panel + 1))


def add_prompt_slot(m):
    """Third prompt line on the select screen: parts button3 / button_text3, one line below
    button2 / button_text2 (the voice screen's layout has the same three)."""
    pos = {}
    a = m["anims"][osbedit.find_anim(m, layout.ice(PROMPT_LAYOUT))]
    for blk in m["blocks"][a["first"]:a["first"] + a["count"]]:
        name = struct.unpack_from("<I", blk["block"], 4)[0]
        pos[name] = blk["tracks"]
    nodes = []
    for tpl, new in (("button2", "button3"), ("button_text2", "button_text3")):
        if layout.ice(new) in pos:
            continue
        y = osbedit.track_value(pos[layout.ice(tpl)], 8)
        nodes.append((layout.ice(tpl), layout.ice(new), {8: y + PROMPT_STEP}))
    if nodes:
        osbedit.add_nodes(m, layout.ice(PROMPT_LAYOUT), nodes)


def portraits(b, classes, report=print):
    """Class portraits in com_pl_icon: classes = {class: {"portrait": png, "portrait_large": png,
    "base": base class}}. A copy of the base class's PlIcon_<base+1> (small) / PlIcon_<base+1>_24
    (large) named MOD_PlIcon_<class> / MOD_PlIcon_<class>_24 shows the new picture.
    -> (new bytes, {class: 1 small | 2 large})."""
    m = osbedit.parse(b)
    fill, has = {}, {}
    for cls, art in sorted(classes.items()):
        for key, suffix, bit in (("portrait", "", 1), ("portrait_large", "_24", 2)):
            if not art.get(key):
                continue
            w, h, img, ox, oy = png_with_origin(art[key])
            mesh = osbedit.add_mesh(m)
            fill[mesh] = image_tiles(w, h, img, ox, oy)
            osbedit.clone_anim(m, layout.ice("PlIcon_%d%s" % (art["base"] + 1, suffix)),
                               layout.ice("MOD_PlIcon_%d%s" % (cls, suffix)), changes={ROOT: {0: mesh}})
            has[cls] = has.get(cls, 0) | bit
    if not has:
        return b, {}
    out = sprites.add_tiles(osbedit.build(m), fill, report)
    return out, has


def bar_faces(b, classes, report=print):
    """The bar party menu's big face in s_o0200: classes = {class: {"bar_face": png, "base": n}}.
    A copy of the base class's PlFace_<base+1> named MOD_PlFace_<class> shows the new picture.
    -> (new bytes, {class: True})."""
    m = osbedit.parse(b)
    fill, has = {}, {}
    for cls, art in sorted(classes.items()):
        if not art.get("bar_face"):
            continue
        w, h, img, ox, oy = png_with_origin(art["bar_face"])
        mesh = osbedit.add_mesh(m)
        fill[mesh] = image_tiles(w, h, img, ox, oy)
        tpl = layout.ice("PlFace_%d" % (art["base"] + 1))
        root, _ = _root(b, tpl)
        osbedit.clone_anim(m, tpl, layout.ice("MOD_PlFace_%d" % cls), changes={root: {0: mesh}})
        has[cls] = True
    if not has:
        return b, {}
    return sprites.add_tiles(osbedit.build(m), fill, report), has


HUD_FACE_NAMES = ["皇女", "戦士", "忍者", "魔女", "巫女", "盗賊"]  # Empress .. Bandit, + 顔01 / 顔02


def hud_faces(b, classes, report=print):
    """Quest HUD faces in cp_o0000: classes = {class: {"hud_face": png, "hud_face2": png, "base": n}}.
    Copies of the base class's <name>顔01 / 顔02 named MOD_HudFace_<class>_1 / _2 show the new
    picture(s) (hud_face2 defaults to hud_face). -> (new bytes, {class: True})."""
    m = osbedit.parse(b)
    fill, has = {}, {}
    for cls, art in sorted(classes.items()):
        if not art.get("hud_face"):
            continue
        for k, key in ((1, "hud_face"), (2, "hud_face2")):
            w, h, img, ox, oy = png_with_origin(art.get(key) or art["hud_face"])
            mesh = osbedit.add_mesh(m)
            fill[mesh] = image_tiles(w, h, img, ox, oy)
            osbedit.clone_anim(m, layout.ice("%s顔%02d" % (HUD_FACE_NAMES[art["base"]], k)),
                               layout.ice("MOD_HudFace_%d_%d" % (cls, k)), changes={ROOT: {0: mesh}})
        has[cls] = True
    if not has:
        return b, {}
    return sprites.add_tiles(osbedit.build(m), fill, report), has


def hud_frames(b, classes, report=print):
    """Quest HUD life frames in cp_o0000: classes = {class: {"hud_frame": png, "base": n}}.
    A copy of the base class's <name>ライフ named MOD_HudFrame_<class> shows the new picture.
    -> (new bytes, {class: True})."""
    m = osbedit.parse(b)
    fill, has = {}, {}
    for cls, art in sorted(classes.items()):
        if not art.get("hud_frame"):
            continue
        w, h, img, ox, oy = png_with_origin(art["hud_frame"])
        mesh = osbedit.add_mesh(m)
        fill[mesh] = image_tiles(w, h, img, ox, oy)
        tpl = layout.ice("%sライフ" % HUD_FACE_NAMES[art["base"]])
        root, _ = _root(b, tpl)
        osbedit.clone_anim(m, tpl, layout.ice("MOD_HudFrame_%d" % cls), changes={root: {0: mesh}})
        has[cls] = True
    if not has:
        return b, {}
    return sprites.add_tiles(osbedit.build(m), fill, report), has


# ---- "empty" pictures -------------------------------------------------------
# One look for "nothing here": the game's own grey "no element" dash icon (com_elic,
# its sixth picture), centred on the spot at a whole-number size (1x to 3x, about
# half the spot) so its pixels stay crisp. Used for empty slots when paging and for a
# class that has no picture of its own for that spot (never its base class's art).
EMPTY_ICON = ("Win/com_elic.osb", 5)
EMPTY_MAX_SCALE = 3


def _dash():
    """(w, h, rgba) of the dash icon."""
    asset, index = EMPTY_ICON
    b = sprites.load(asset)
    a = sprites.animations(b)[index]
    mesh = sprites.frames(a)[0][1]
    w, h, imgs = sprites.render(b, [mesh])
    return w, h, imgs[0]


def _root(b, anim_hash):
    """(root node name hash, picture/mesh) of an animation."""
    for a in sprites.animations(b):
        if a["hash"] == anim_hash:
            fr = sprites.frames(a)
            if not fr:
                raise ValueError("animation %08x has no picture" % anim_hash)
            return a["nodes"][0]["name"], fr[0][1]
    raise ValueError("no animation %08x" % anim_hash)


def empty_picture(b, mesh, silhouette=None):
    """(w, h, img, ox, oy): the dash centred on the canvas of picture `mesh`. silhouette =
    (r, g, b, a) also fills the picture's own shape with that colour first (a panel that
    keeps its outline but shows no character)."""
    x0, y1, w, h = sprites.canvas(b, [mesh])
    img = bytearray(w * h * 4)
    if silhouette:
        _, _, imgs = sprites.render(b, [mesh], box=(x0, y1, w, h))
        src = imgs[0]
        for i in range(0, len(src), 4):
            if src[i + 3]:
                img[i:i + 4] = bytes(silhouette[:3]) + bytes((src[i + 3] * silhouette[3] // 255,))
    dw, dh, dash = _dash()
    s = max(1, min(EMPTY_MAX_SCALE, min(w, h) // 2 // max(dw, dh)))
    left, top = (w - dw * s) // 2, (h - dh * s) // 2
    for y in range(dh * s):
        for x in range(dw * s):
            o2 = ((y // s) * dw + x // s) * 4
            p = dash[o2:o2 + 4]
            if p[3] and 0 <= left + x < w and 0 <= top + y < h:
                o = ((top + y) * w + left + x) * 4
                img[o:o + 4] = p
    return w, h, bytes(img), x0, y1


# The quest HUD life frame has no "picture" to replace with the dash: its empty version is a
# plain grey bevelled frame of the same layout (a box for the face, a housing round the
# life bar, a tab behind "LV."), keeping the template's black bar slot and "LV." exactly.
# Rectangles are in the template's canvas pixels (the frame is drawn in 2x2 blocks).
FRAME_SHAPES = [(0, 4, 62, 90), (56, 30, 290, 62), (78, 14, 108, 32)]
FRAME_KEEP = [(80, 16, 106, 32), (60, 32, 288, 60)]  # where the template's own black / white stays
FRAME_GREYS = {"edge": (36, 36, 44), "light": (196, 196, 200), "fill": (132, 132, 138),
               "shadow": (84, 84, 90)}


def empty_frame(b, mesh):
    """(w, h, img, ox, oy): the grey bevelled version of life frame picture `mesh`."""
    x0, y1, w, h = sprites.canvas(b, [mesh])
    _, _, imgs = sprites.render(b, [mesh], box=(x0, y1, w, h))
    src = imgs[0]
    S = 2  # block size
    bw, bh = (w + S - 1) // S, (h + S - 1) // S
    inside = [[any(x0_ <= bx * S < x1_ and y0_ <= by * S < y1_ for x0_, y0_, x1_, y1_ in FRAME_SHAPES)
               for bx in range(bw)] for by in range(bh)]
    at = lambda bx, by: 0 <= bx < bw and 0 <= by < bh and inside[by][bx]
    img = bytearray(w * h * 4)
    for by in range(bh):
        for bx in range(bw):
            if not inside[by][bx]:
                continue
            if not (at(bx - 1, by) and at(bx + 1, by) and at(bx, by - 1) and at(bx, by + 1)):
                c = FRAME_GREYS["edge"]
            elif not (at(bx - 2, by) and at(bx, by - 2)):
                c = FRAME_GREYS["light"]
            elif not (at(bx + 2, by) and at(bx, by + 2)):
                c = FRAME_GREYS["shadow"]
            else:
                c = FRAME_GREYS["fill"]
            for y in range(by * S, min(h, by * S + S)):
                for x in range(bx * S, min(w, bx * S + S)):
                    o = (y * w + x) * 4
                    img[o:o + 4] = bytes(c) + bytes((255,))
    for kx0, ky0, kx1, ky1 in FRAME_KEEP:
        for y in range(ky0, min(h, ky1)):
            for x in range(kx0, min(w, kx1)):
                o = (y * w + x) * 4
                p = src[o:o + 4]
                if p[3] and (max(p[:3]) < 40 or min(p[:3]) > 200):
                    img[o:o + 4] = p
    return w, h, bytes(img), x0, y1


def add_empties(b, names, report=print):
    """For each (template animation name, new name[, "frame"]) in this sprite bank: a copy
    of the template showing the empty picture (or the grey life frame). -> new bytes."""
    m = osbedit.parse(b)
    fill = {}
    for entry in names:
        tpl, new = entry[0], entry[1]
        h = layout.ice(tpl)
        if osbedit.find_anim(m, layout.ice(new)) >= 0:
            continue
        root, mesh = _root(b, h)
        paint = empty_frame if entry[2:] == ("frame",) else empty_picture
        w, hh, img, ox, oy = paint(b, mesh)
        nm = osbedit.add_mesh(m)
        fill[nm] = image_tiles(w, hh, img, ox, oy)
        osbedit.clone_anim(m, h, layout.ice(new), changes={root: {0: nm}})
    if not fill:
        return b
    return sprites.add_tiles(osbedit.build(m), fill, report)


def build(b, classes, report=print):
    """b = s_o0a00 (decoded); classes = {class: {"panel": png path or None, "highlight": png or None}}.
    -> (new bytes, {class: has highlight}, {page: has layout})."""
    m = osbedit.parse(b)
    fill = {}
    highlights, pages = {}, {}
    page_panels = {}
    empty = {}  # panel -> its empty picture (the panel's outline, dark, with the dash)

    def empty_mesh(k):
        if k not in empty:
            w, h, img, ox, oy = empty_picture(b, panel_mesh(m, k), silhouette=EMPTY_PANEL)
            empty[k] = osbedit.add_mesh(m)
            fill[empty[k]] = image_tiles(w, h, img, ox, oy)
        return empty[k]

    for cls, art in sorted(classes.items()):
        page, panel = cls // 6, cls % 6
        mesh = osbedit.add_mesh(m)  # the selected-panel picture (none drawn when it has no art)
        if art.get("highlight"):
            w, h, img, ox, oy = png_with_origin(art["highlight"])
            fill[mesh] = image_tiles(w, h, img, ox, oy)
        osbedit.clone_anim(m, layout.ice(HIGHLIGHTS[panel]), layout.ice("MOD_SEL_%d" % cls),
                           changes={ROOT: {0: mesh}})
        highlights[cls] = True
        if art.get("panel"):
            w, h, img, ox, oy = png_with_origin(art["panel"])
            mesh = osbedit.add_mesh(m)
            fill[mesh] = image_tiles(w, h, img, ox, oy)
            page_panels.setdefault(page, {})[panel] = mesh
        else:
            page_panels.setdefault(page, {})[panel] = empty_mesh(panel)
    npages = max(classes) // 6 + 1 if classes else 1
    empty_on = {}
    for page in range(1, npages):
        panels = page_panels.setdefault(page, {})
        for k in range(6):
            if 6 * page + k not in classes:
                panels[k] = empty_mesh(k)
                empty_on.setdefault(page, []).append(k)
    for page, panels in sorted(page_panels.items()):
        if not panels:
            continue
        changes = {layout.ice("PL%d" % (k + 1)): {0: mesh} for k, mesh in panels.items()}
        for k in empty_on.get(page, []):
            changes[layout.ice("PL%d_Lv" % (k + 1))] = {8: -2000.0}  # no level shown: off screen
        osbedit.clone_anim(m, layout.ice(LAYOUT), layout.ice("MOD_SEL_PAGE%d" % page), changes=changes)
        pages[page] = True
    # the info panel shows the same layout (its COLOR icon hangs on it) and must not be
    # switched later: from the start it gets a copy without panel pictures, so only
    # the switched panel layout draws them
    blank = osbedit.add_mesh(m)
    osbedit.clone_anim(m, layout.ice(LAYOUT), layout.ice("MOD_SEL_INFO"),
                       changes={layout.ice("PL%d" % (k + 1)): {0: blank} for k in range(6)})
    add_prompt_slot(m)
    out = osbedit.build(m)
    if fill:
        out = sprites.add_tiles(out, fill, report)
    return out, highlights, pages
