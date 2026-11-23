# LiDE50 Scan - turns TROUBLESHOOTING.md into Troubleshooting.html: one self-contained page in
# the app's look (pictures embedded), which ships next to LiDE50Scan.exe and opens from the
# app's ⋯ menu without a network connection.
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the LiDE50 Scan contributors
#
#   python3 docs/troubleshooting/build_html.py TROUBLESHOOTING.md dist/Troubleshooting.html
#
# Understands the Markdown that the guide uses: headings, paragraphs, lists, block quotes,
# block images, **bold**, *italic*, `code` and [links](url). Needs Pillow for the pictures.
import base64
import html
import io
import os
import re
import sys

from PIL import Image


def slug(title):
    """GitHub's heading anchors, so links work the same on GitHub and in the page."""
    s = title.strip().lower()
    s = re.sub(r"[^\w\- ]", "", s)
    return s.replace(" ", "-")


def inline(text):
    t = html.escape(text, quote=False)
    t = re.sub(r"`([^`]+)`", r"<code>\1</code>", t)
    t = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", t)
    t = re.sub(r"(?<![\w*])\*([^*]+)\*(?![\w*])", r"<em>\1</em>", t)

    def link(m):
        label, url = m.group(1), m.group(2)
        ext = url.startswith("http")
        return f'<a href="{html.escape(url)}"{" target=\"_blank\" rel=\"noopener\"" if ext else ""}>{label}</a>'
    return re.sub(r"\[([^\]]+)\]\(([^)]+)\)", link, t)


def picture(path, base_dir):
    """An image as a data URI: WebP, which is much smaller than PNG for these pictures."""
    im = Image.open(os.path.join(base_dir, path))
    im.load()
    buf = io.BytesIO()
    if im.mode not in ("RGB", "RGBA"):
        im = im.convert("RGBA")
    im.save(buf, "WEBP", quality=90, method=6)
    w, h = im.size
    return "data:image/webp;base64," + base64.b64encode(buf.getvalue()).decode(), w, h


def convert(md, base_dir):
    lines = md.splitlines()
    out, toc = [], []
    title = "Troubleshooting"
    i = 0
    in_section = False
    para = []

    def flush_para():
        # text before the first section is the GitHub page's introduction; the page has its own
        if para and in_section:
            out.append("<p>" + inline(" ".join(para)) + "</p>")
        para.clear()

    while i < len(lines):
        line = lines[i]
        s = line.strip()
        if not s:
            flush_para()
            i += 1
            continue
        if s.startswith("# "):
            flush_para()
            title = s[2:].strip()
            i += 1
            continue
        if s.startswith("## "):
            flush_para()
            if in_section:
                out.append("</section>")
            h = s[3:].strip()
            sid = slug(h)
            toc.append((sid, h))
            out.append(f'<section class="card" id="{sid}"><h2><a class="anchor" href="#{sid}">{inline(h)}</a></h2>')
            in_section = True
            i += 1
            continue
        if s == "---":
            flush_para()
            i += 1
            continue
        m = re.match(r"!\[([^\]]*)\]\(([^)]+)\)$", s)
        if m:
            flush_para()
            uri, w, h = picture(m.group(2), base_dir)
            # the pictures are drawn at 1.5x (illustrations) or 1x (screenshots)
            disp = w / 1.5 if w >= 1300 else w
            out.append(f'<figure><img src="{uri}" alt="{html.escape(m.group(1))}" width="{int(disp)}" '
                       f'loading="lazy"></figure>')
            i += 1
            continue
        if s.startswith(">"):
            flush_para()
            buf = []
            while i < len(lines) and lines[i].strip().startswith(">"):
                buf.append(lines[i].strip()[1:].strip())
                i += 1
            out.append('<aside class="tip">' + inline(" ".join(buf)) + "</aside>")
            continue
        m = re.match(r"(\d+)\. (.*)", s)
        if m or s.startswith("- "):
            flush_para()
            ordered = bool(m)
            items = []
            while i < len(lines):
                t = lines[i]
                st = t.strip()
                mm = re.match(r"(\d+)\. (.*)", st)
                if (ordered and mm) or (not ordered and st.startswith("- ")):
                    items.append(mm.group(2) if ordered else st[2:])
                elif st and t.startswith(" ") and items:
                    items[-1] += " " + st
                else:
                    break
                i += 1
            tag = "ol" if ordered else "ul"
            # the contents list at the top becomes the page's navigation instead
            if ordered and all(re.match(r"\[.*\]\(#", it) for it in items):
                continue
            out.append(f"<{tag}>" + "".join(f"<li>{inline(it)}</li>" for it in items) + f"</{tag}>")
            continue
        if s.startswith("**") and s.endswith("**") and s.count("**") == 2:
            flush_para()   # "**Contents**": the navigation replaces it
            i += 1
            continue
        para.append(s)
        i += 1
    flush_para()
    if in_section:
        out.append("</section>")
    return title, toc, "\n".join(out)


CSS = r"""
:root{
  --bg1:#c3dcf8;--bg2:#dcd3f4;--bg3:#f5cfe3;--blobA:#a9e4dc;--blobB:#f7b9d6;
  --glass:rgba(255,255,255,.70);--glass2:rgba(255,255,255,.52);--rim:rgba(255,255,255,.95);
  --text:#1c1f2a;--text2:#555b6c;--text3:#8d93a3;--accent:#1f7aff;--accent2:#6a5cf2;
  --code:rgba(31,122,255,.10);--codeText:#1a5fd1;--shadow:0 18px 40px -18px rgba(40,44,90,.35);
}
@media (prefers-color-scheme: dark){:root:not([data-theme=light]){
  --bg1:#141a2e;--bg2:#1d1a33;--bg3:#2a1b2e;--blobA:#1f4e57;--blobB:#5a2547;
  --glass:rgba(34,38,56,.72);--glass2:rgba(34,38,56,.55);--rim:rgba(255,255,255,.10);
  --text:#eef0f6;--text2:#b5bacb;--text3:#80869a;--code:rgba(108,150,255,.16);--codeText:#9ec0ff;
  --shadow:0 18px 40px -18px rgba(0,0,0,.7);}}
:root[data-theme=dark]{
  --bg1:#141a2e;--bg2:#1d1a33;--bg3:#2a1b2e;--blobA:#1f4e57;--blobB:#5a2547;
  --glass:rgba(34,38,56,.72);--glass2:rgba(34,38,56,.55);--rim:rgba(255,255,255,.10);
  --text:#eef0f6;--text2:#b5bacb;--text3:#80869a;--code:rgba(108,150,255,.16);--codeText:#9ec0ff;
  --shadow:0 18px 40px -18px rgba(0,0,0,.7);}
*{box-sizing:border-box}
html{scroll-behavior:smooth;scroll-padding-top:24px}
body{margin:0;font:16px/1.6 Inter,"Segoe UI Variable Text","Segoe UI",system-ui,sans-serif;color:var(--text);
  background:linear-gradient(135deg,var(--bg1),var(--bg2) 55%,var(--bg3));background-attachment:fixed;min-height:100vh}
body:before,body:after{content:"";position:fixed;z-index:-1;border-radius:50%;filter:blur(60px);opacity:.75}
body:before{width:46vw;height:46vw;left:-10vw;top:-12vw;background:var(--blobA)}
body:after{width:40vw;height:40vw;right:-8vw;bottom:-14vw;background:var(--blobB)}
.layout{display:grid;grid-template-columns:280px minmax(0,1fr);gap:28px;max-width:1240px;margin:0 auto;padding:28px}
@media (max-width:900px){.layout{grid-template-columns:1fr;padding:16px}nav{position:static!important;max-height:none!important}}
.glass{background:var(--glass);backdrop-filter:blur(22px) saturate(1.4);-webkit-backdrop-filter:blur(22px) saturate(1.4);
  border:1px solid var(--rim);border-radius:26px;box-shadow:var(--shadow)}
nav{position:sticky;top:28px;align-self:start;max-height:calc(100vh - 56px);overflow:auto;padding:22px 18px}
.brand{display:flex;align-items:center;gap:12px;margin:0 4px 16px}
.logo{width:40px;height:40px;border-radius:12px;background:linear-gradient(90deg,var(--accent),var(--accent2));
  box-shadow:0 8px 18px -6px rgba(31,122,255,.55);display:grid;place-items:center}
.brand b{display:block;font-size:17px}.brand span{font-size:12.5px;color:var(--text2)}
.search{width:100%;padding:10px 14px;border-radius:14px;border:1px solid var(--rim);background:var(--glass2);color:var(--text);
  font:inherit;font-size:14px;outline:none;margin-bottom:12px}
.search:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(31,122,255,.18)}
nav ol{list-style:none;margin:0;padding:0;counter-reset:n}
nav li{counter-increment:n}
nav li a{display:flex;gap:10px;padding:7px 10px;border-radius:12px;color:var(--text);text-decoration:none;font-size:14px;line-height:1.35}
nav li a:before{content:counter(n);flex:0 0 22px;height:22px;border-radius:11px;background:var(--code);color:var(--codeText);
  font-size:12px;font-weight:700;display:grid;place-items:center}
nav li a:hover,nav li a.on{background:var(--glass2)}
nav li a.on{box-shadow:inset 0 0 0 1px var(--rim)}
nav li.hide{display:none}
.theme{margin-top:14px;width:100%;padding:9px;border-radius:14px;border:1px solid var(--rim);background:var(--glass2);
  color:var(--text);font:inherit;font-size:13.5px;cursor:pointer}
header.hero{padding:30px 34px;margin-bottom:22px}
header.hero h1{margin:0 0 6px;font-size:30px;letter-spacing:-.01em}
header.hero p{margin:0;color:var(--text2)}
.card{padding:26px 34px 28px;margin-bottom:22px;background:var(--glass);backdrop-filter:blur(22px) saturate(1.4);
  -webkit-backdrop-filter:blur(22px) saturate(1.4);border:1px solid var(--rim);border-radius:26px;box-shadow:var(--shadow)}
.card.hide{display:none}
h2{margin:0 0 14px;font-size:22px;letter-spacing:-.01em}
h2 a{color:inherit;text-decoration:none}
p,li{color:var(--text2)}strong{color:var(--text)}
a{color:var(--accent)}
code{background:var(--code);color:var(--codeText);padding:2px 8px;border-radius:8px;font:600 .86em ui-monospace,"Cascadia Code",Consolas,monospace;white-space:nowrap}
mark{background:rgba(255,214,90,.55);color:inherit;border-radius:4px}
ol,ul{padding-left:1.4em}li{margin:6px 0}
ol>li::marker{color:var(--accent);font-weight:700}
figure{margin:18px 0}
figure img{display:block;max-width:100%;height:auto;border-radius:20px;box-shadow:var(--shadow)}
aside.tip{margin:0 0 22px;padding:18px 22px 18px 58px;position:relative;color:var(--text2);
  background:var(--glass);border:1px solid var(--rim);border-radius:22px;box-shadow:var(--shadow)}
aside.tip:before{content:"i";position:absolute;left:18px;top:18px;width:26px;height:26px;border-radius:13px;
  background:linear-gradient(90deg,var(--accent),var(--accent2));color:#fff;font-weight:800;display:grid;place-items:center;font-size:14px}
.empty{display:none;padding:22px 30px;color:var(--text2)}
footer{color:var(--text3);font-size:13px;text-align:center;padding:8px 0 30px}
@media print{body:before,body:after,nav{display:none}.layout{display:block}.card,.glass{box-shadow:none;backdrop-filter:none;background:#fff}}
"""

JS = r"""
(function(){
  var root=document.documentElement, btn=document.getElementById('theme');
  function store(k,v){try{v===undefined?localStorage.removeItem(k):localStorage.setItem(k,v)}catch(e){}}
  function load(k){try{return localStorage.getItem(k)}catch(e){return null}}
  var t=load('lide50-guide-theme'); if(t) root.setAttribute('data-theme',t);
  function label(){var d=root.getAttribute('data-theme')||(matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light');
    btn.textContent=d==='dark'?'Light mode':'Dark mode';}
  btn.onclick=function(){var d=root.getAttribute('data-theme')||(matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light');
    d=d==='dark'?'light':'dark';root.setAttribute('data-theme',d);store('lide50-guide-theme',d);label();};
  label();
  // filter the sections by a code or a word, and highlight the matches
  var q=document.getElementById('q'), cards=[].slice.call(document.querySelectorAll('section.card')),
      links=[].slice.call(document.querySelectorAll('nav li')), empty=document.getElementById('empty');
  function unmark(node){
    [].slice.call(node.querySelectorAll('mark')).forEach(function(m){
      var p=m.parentNode; p.replaceChild(document.createTextNode(m.textContent),m); p.normalize();});
  }
  function mark(node,v){
    var w=document.createTreeWalker(node,NodeFilter.SHOW_TEXT,null),list=[],n;
    while((n=w.nextNode()))if(n.nodeValue.toLowerCase().indexOf(v)>=0)list.push(n);
    list.forEach(function(t){
      var i=t.nodeValue.toLowerCase().indexOf(v),mid=t.splitText(i),m=document.createElement('mark');
      mid.splitText(v.length); m.textContent=mid.nodeValue; mid.parentNode.replaceChild(m,mid);});
  }
  q.oninput=function(){
    var v=q.value.trim().toLowerCase(), shown=0;
    cards.forEach(function(c,i){
      unmark(c);
      var hit=!v||c.textContent.toLowerCase().indexOf(v)>=0;
      c.classList.toggle('hide',!hit); links[i].classList.toggle('hide',!hit);
      if(hit&&v){shown++;mark(c,v);}
    });
    empty.style.display=v&&!shown?'block':'none';
  };
  // the contents entry of the section in view
  if('IntersectionObserver' in window){
    var byId={};
    links.forEach(function(l){var a=l.querySelector('a');byId[a.getAttribute('href').slice(1)]=a;});
    var io=new IntersectionObserver(function(es){es.forEach(function(e){
      if(!e.isIntersecting)return;
      for(var k in byId)byId[k].classList.remove('on');
      if(byId[e.target.id])byId[e.target.id].classList.add('on');});},{rootMargin:'-30% 0px -60% 0px'});
    cards.forEach(function(c){io.observe(c);});
  }
})();
"""

LOGO = ('<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="#fff" stroke-width="1.8" '
        'stroke-linecap="round"><rect x="4" y="3" width="16" height="18" rx="3"/><path d="M8 9h8M8 13h8"/></svg>')


def page(title, toc, body, version):
    nav = "".join(f'<li><a href="#{sid}">{inline(h)}</a></li>' for sid, h in toc)
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>{html.escape(title)} - LiDE50 Scan</title>
<style>{CSS}</style>
</head>
<body>
<div class="layout">
<nav class="glass" aria-label="Contents">
  <div class="brand"><div class="logo">{LOGO}</div><div><b>LiDE50 Scan</b><span>Troubleshooting{(" · " + version) if version else ""}</span></div></div>
  <input class="search" id="q" type="search" placeholder="Search for a code or a word" aria-label="Search">
  <ol>{nav}</ol>
  <button class="theme" id="theme" type="button">Dark mode</button>
</nav>
<main>
<header class="hero glass"><h1>{html.escape(title)}</h1>
<p>Find what you're seeing below, or type the code from a message (such as <code>CAL-NO-LIGHT</code>) into the search box.</p></header>
<div class="card empty" id="empty">Nothing matches. Try a shorter word, or the code shown at the bottom of the message.</div>
{body}
<footer>LiDE50 Scan by iFloofie · github.com/ifloofie/LiDE50-scanner · GPL-2.0-or-later</footer>
</main>
</div>
<script>{JS}</script>
</body>
</html>
"""


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "TROUBLESHOOTING.md"
    dst = sys.argv[2] if len(sys.argv) > 2 else "Troubleshooting.html"
    version = sys.argv[3] if len(sys.argv) > 3 else ""
    with open(src, encoding="utf-8") as f:
        md = f.read()
    title, toc, body = convert(md, os.path.dirname(os.path.abspath(src)))
    with open(dst, "w", encoding="utf-8") as f:
        f.write(page(title, toc, body, version))
    print(f"{dst}: {os.path.getsize(dst) / 1024:.0f} KB, {len(toc)} sections")


if __name__ == "__main__":
    main()
