"""Render docs/user-manual.md to docs/user-manual.pdf in the panel's marine-blue style.

    python app/tools/manual/make-manual-pdf.py [--browser PATH]

Needs the `markdown` package (pip install markdown) and Microsoft Edge or Chrome (headless
print-to-PDF). Links to other docs become links to the GitHub repository."""
import argparse, base64, os, re, subprocess, sys, tempfile, time
import markdown

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
REPO = "https://github.com/JSFrouws/ddcs-4.1-Sea-Otter/blob/main/docs/"
SHOT = os.path.join(ROOT, "docs", "img", "run-screen.png")

CSS = """
@page { size: A4; margin: 0; }
* { -webkit-print-color-adjust: exact; print-color-adjust: exact; box-sizing: border-box; }
html, body { margin: 0; background: #06172f; }
body { font-family: 'Segoe UI', 'Barlow Semi Condensed', Arial, sans-serif; color: #e8f0fb;
       font-size: 10.5pt; line-height: 1.45; }
.page { padding: 0 17mm; }
.cover { height: 297mm; padding: 0 17mm; display: flex; flex-direction: column; justify-content: center;
         background: linear-gradient(180deg, #14407a 0%, #0a2244 55%, #06172f 100%); page-break-after: always; }
.cover .tag { display: inline-block; border: 1px solid #5cc8ff; color: #5cc8ff; padding: 2px 10px;
              font-size: 10pt; letter-spacing: 2px; border-radius: 3px; width: max-content; }
.cover h1 { font-size: 34pt; margin: 14px 0 6px; color: #fff; border: 0; padding: 0; background: none; }
.cover .sub { color: #8fa7ca; font-size: 14pt; }
.cover img { width: 100%; margin-top: 18mm; border: 1px solid #355f99; border-radius: 4px; }
.cover .foot { margin-top: 10mm; color: #5d789f; font-size: 9pt; }
h1 { font-size: 20pt; color: #fff; }
h2 { font-size: 14.5pt; color: #fff; background: linear-gradient(90deg, #1f5a86, #0a2244);
     border-left: 4px solid #5cc8ff; padding: 5px 10px; margin: 20px 0 10px; border-radius: 2px;
     page-break-after: avoid; }
h3 { font-size: 11.5pt; color: #5cc8ff; margin: 14px 0 6px; page-break-after: avoid; }
a { color: #3ddbb0; text-decoration: none; }
strong { color: #fff; }
em { color: #c2d6f0; }
code { font-family: 'JetBrains Mono', Consolas, monospace; font-size: 9pt; color: #ffb547;
       background: #0f2d57; padding: 0 3px; border-radius: 2px; }
pre { background: #0a2244; border: 1px solid #244778; padding: 8px 10px; border-radius: 3px;
      white-space: pre-wrap; page-break-inside: avoid; }
pre code { color: #c2d6f0; background: none; padding: 0; }
table { border-collapse: collapse; width: 100%; margin: 8px 0 12px; font-size: 9.5pt;
        page-break-inside: avoid; }
th { background: #14407a; color: #fff; text-align: left; padding: 5px 7px; border: 1px solid #355f99; }
td { padding: 4px 7px; border: 1px solid #244778; vertical-align: top; background: #0a2244; }
tr:nth-child(even) td { background: #0f2d57; }
blockquote { margin: 10px 0; padding: 8px 12px; border-left: 4px solid #ffb547;
             background: #2a2210; color: #f5dfb8; border-radius: 2px; }
blockquote strong { color: #ffb547; }
hr { border: 0; border-top: 1px solid #244778; margin: 14px 0; }
ul, ol { padding-left: 20px; }
li { margin: 2px 0; }
table.wrap, table.wrap > * > tr > td { border: 0; background: none !important; padding: 0; margin: 0; }
table.wrap { page-break-inside: auto; font-size: inherit; }
.sp-top { height: 13mm; } .sp-bot { height: 12mm; }
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--browser", default=None)
    a = ap.parse_args()
    src = open(os.path.join(ROOT, "docs", "user-manual.md"), encoding="utf-8").read()
    title_line, body = src.split("\n", 1)
    # links to other docs -> the GitHub copy
    body = re.sub(r"\]\((?!https?:)([^)#]+\.md)(#[^)]*)?\)", lambda m: "](" + REPO + m.group(1) + (m.group(2) or "") + ")", body)
    html_body = markdown.markdown(body, extensions=["tables", "fenced_code"])
    shot = ""
    if os.path.exists(SHOT):
        shot = '<img src="data:image/png;base64,%s">' % base64.b64encode(open(SHOT, "rb").read()).decode()
    html = f"""<!doctype html><html><head><meta charset="utf-8"><title>Pandora user manual</title>
<style>{CSS}</style></head><body>
<section class="cover"><span class="tag">DDCS V4.1 · SEA OTTER</span>
<h1>User manual</h1><div class="sub">Replacement controller software for the DDCS V4.1 — daily use at the machine</div>
{shot}
<div class="foot">github.com/JSFrouws/ddcs-4.1-Sea-Otter · docs/user-manual.md</div></section>
<table class="wrap"><thead><tr><td><div class="sp-top"></div></td></tr></thead>
<tfoot><tr><td><div class="sp-bot"></div></td></tr></tfoot>
<tbody><tr><td><div class="page">{html_body}</div></td></tr></tbody></table></body></html>"""
    tmp = os.path.join(tempfile.gettempdir(), "pandora-manual.html")
    open(tmp, "w", encoding="utf-8").write(html)
    out = os.path.join(ROOT, "docs", "user-manual.pdf")
    if os.path.exists(out): os.remove(out)                 # so a silent browser failure shows
    browsers = [a.browser] if a.browser else [
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Google\Chrome\Application\chrome.exe", "google-chrome", "chromium"]
    for b in browsers:
        if b and (os.path.exists(b) or not os.path.isabs(b)):
            try:
                prof = tempfile.mkdtemp(prefix="pandora-manual-")   # own instance: a running browser would swallow the job
                subprocess.run([b, "--headless=new", f"--user-data-dir={prof}", "--disable-gpu", "--no-pdf-header-footer",
                                "--print-to-pdf-no-header", f"--print-to-pdf={out}", "file:///" + tmp.replace("\\", "/")],
                               check=True, timeout=120, capture_output=True)
                for _ in range(30):                        # the browser may finish writing after it returns
                    if os.path.exists(out) and os.path.getsize(out) > 0: break
                    time.sleep(0.5)
                if os.path.exists(out):
                    print("wrote", out)
                    return
                print("browser made no PDF:", b, file=sys.stderr)
            except (OSError, subprocess.SubprocessError) as e:
                print("browser failed:", b, e, file=sys.stderr)
    sys.exit("no browser for print-to-PDF found")


if __name__ == "__main__":
    main()
