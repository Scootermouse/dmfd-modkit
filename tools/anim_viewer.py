"""Make an HTML page that plays every animation of an exported body, numbered, so you can see
which animation is which (idle, run, shoot...) before redrawing frames.

    python -I tools/anim_viewer.py mod/sprites_original/pl0_cb00 out.html

Export the frames first (Export sprites.bat). The page links to the PNGs where they are, so keep
it next to them or pass the path you want; open it in any browser.
"""
import json
import os
import re
import sys

NAME = re.compile(r"^a(\d+)_f(\d+)_m\d+_o(-?\d+)_(-?\d+)\.png$")

PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>%(title)s animations</title>
<style>
:root { --bg: #f2f1ee; --card: #fff; --ink: #222; --muted: #666; --line: #ddd; }
@media (prefers-color-scheme: dark) { :root { --bg: #1d1d1f; --card: #2a2a2d; --ink: #eee; --muted: #aaa; --line: #3a3a3e; } }
body { margin: 0; padding: 16px; background: var(--bg); color: var(--ink); font: 14px/1.4 system-ui, sans-serif; }
h1 { font-size: 18px; margin: 0 0 4px; }
p { margin: 0 0 12px; color: var(--muted); max-width: 70ch; }
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 8px; }
.card { background: var(--card); border: 1px solid var(--line); border-radius: 6px; padding: 6px; text-align: center; }
.card b { font-variant-numeric: tabular-nums; }
.card span { color: var(--muted); font-size: 12px; }
canvas { image-rendering: pixelated; width: 100%%; height: auto; display: block; background: repeating-conic-gradient(#0000 0 25%%, #8881 0 50%%) 0 0 / 16px 16px; }
label { color: var(--muted); }
</style></head><body>
<h1>%(title)s: %(n)d animations, %(frames)d frames</h1>
<p>Each card plays one animation; the number is the <code>aNNN</code> at the start of its frame files.
Frames are drawn at their real positions (the cross is the character's anchor point).</p>
<p><label><input type="range" id="speed" min="2" max="30" value="8"> frames per second</label></p>
<div class="grid" id="grid"></div>
<script>
const DIR = %(dir)s, ANIMS = %(anims)s;
const grid = document.getElementById('grid');
let fps = 8;
document.getElementById('speed').oninput = e => fps = +e.target.value;
const players = [];
for (const [num, frames] of ANIMS) {
  const card = document.createElement('div'); card.className = 'card';
  const c = document.createElement('canvas');
  card.appendChild(c);
  card.insertAdjacentHTML('beforeend', `<b>a${num}</b> <span>${frames.length} frame${frames.length > 1 ? 's' : ''}</span>`);
  grid.appendChild(card);
  const imgs = frames.map(f => { const i = new Image(); i.src = DIR + f[0]; return i; });
  let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
  Promise.all(imgs.map(i => i.decode().catch(() => {}))).then(() => {
    frames.forEach((f, k) => { x0 = Math.min(x0, f[1]); y1 = Math.max(y1, f[2]);
      x1 = Math.max(x1, f[1] + imgs[k].width); y0 = Math.min(y0, f[2] - imgs[k].height); });
    c.width = x1 - x0; c.height = y1 - y0;
    players.push({ c, imgs, frames, x0, y1, k: 0 });
  });
}
let last = 0;
function tick(t) {
  if (t - last > 1000 / fps) {
    last = t;
    for (const p of players) {
      const g = p.c.getContext('2d'), f = p.frames[p.k];
      g.clearRect(0, 0, p.c.width, p.c.height);
      g.drawImage(p.imgs[p.k], f[1] - p.x0, p.y1 - f[2]);
      g.fillStyle = '#e33'; g.fillRect(-p.x0 - 3, p.y1, 7, 1); g.fillRect(-p.x0, p.y1 - 3, 1, 7);
      p.k = (p.k + 1) %% p.frames.length;
    }
  }
  requestAnimationFrame(tick);
}
requestAnimationFrame(tick);
</script></body></html>
"""


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    src, out = os.path.abspath(argv[1]), os.path.abspath(argv[2])
    anims = {}
    for f in sorted(os.listdir(src)):
        m = NAME.match(f)
        if m:
            anims.setdefault(m.group(1), []).append((f, int(m.group(3)), int(m.group(4))))
    rel = os.path.relpath(src, os.path.dirname(out)).replace(os.sep, "/") + "/"
    html = PAGE % {"title": os.path.basename(src), "n": len(anims), "frames": sum(len(v) for v in anims.values()),
                   "dir": json.dumps(rel), "anims": json.dumps(sorted(anims.items()))}
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "w", encoding="utf-8").write(html)
    print("%s: %d animations" % (out, len(anims)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
