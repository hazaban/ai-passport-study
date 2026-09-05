# -*- coding: utf-8 -*-
"""Markdown -> HTML(含目录) -> PDF, 使用 weasyprint 渲染, 支持目录点击跳转。"""
import sys
import markdown
from weasyprint import HTML

SRC = "/workspace/docs/408知识全景清单.md"
OUT = "/workspace/docs/408知识全景清单.pdf"

with open(SRC, "r", encoding="utf-8") as f:
    text = f.read()

md = markdown.Markdown(extensions=["toc", "fenced_code", "tables", "sane_lists"],
                       extension_configs={"toc": {"toc_depth": "1-3", "permalink": False}})
body = md.convert(text)
toc = md.toc  # 生成的 <div class="toc">...</div>

CSS = r"""
@page {
  size: A4;
  margin: 1.6cm 1.5cm 1.8cm 1.5cm;
  @bottom-center { content: counter(page) " / " counter(pages);
                   font-family: "Noto Sans CJK SC","DejaVu Sans"; font-size: 8pt; color: #888; }
}
html { -weasy-hyphens: none; }
body {
  font-family: "Noto Sans CJK SC","DejaVu Sans",sans-serif;
  font-size: 10.5pt; line-height: 1.65; color: #1b1b1b;
  margin: 0;
}
h1,h2,h3,h4 { font-family: "Noto Sans CJK SC","DejaVu Sans",sans-serif;
              color: #0d3b66; line-height: 1.3; }
h1 { font-size: 17pt; border-bottom: 3px solid #0d3b66; padding-bottom: 6px;
     margin-top: 0; }
h2 { font-size: 14pt; border-bottom: 1.5px solid #92b4d4; padding-bottom: 4px; }
h3 { font-size: 11.5pt; color: #14507a; }
h4 { font-size: 10.8pt; color: #333; }

/* 每一大部分(h1)另起一页；TOC 内不强制分页 */
.content > h1 { break-before: page; }

pre, code { font-family: "Noto Sans CJK SC","DejaVu Sans Mono",monospace; }
p code, li code { background:#f2f4f7; padding:0 3px; border-radius:3px; }
pre {
  background:#f6f8fa; border:1px solid #d9dee3; border-radius:5px;
  padding:8px 10px; font-size:8.8pt; line-height:1.45; white-space:pre-wrap;
  word-break:break-word; overflow-wrap:break-word;
}
blockquote { margin:8px 0; padding:4px 14px; border-left:4px solid #b0c4de;
             background:#f6f8fb; color:#333; }
table { border-collapse:collapse; width:100%; margin:8px 0; font-size:9.3pt;
        table-layout:fixed; }
th,td { border:1px solid #b9c4ce; padding:3px 6px; vertical-align:top;
        overflow-wrap:break-word; word-break:break-word; }
th { background:#e8eef5; text-align:left; }
tr:nth-child(even) td { background:#fafbfc; }
a { color: #0d3b66; }

/* -------- 目录 -------- */
header.title { text-align:center; padding:10pt 0 6pt 0; border-bottom:none; }
header.title h1.t { border:none; font-size:22pt; margin:0; }
header.title .sub { color:#666; font-size:11pt; margin-top:4pt; }
.tocbox { width:100%; }
.toc > ul { list-style:none; padding-left:0; margin:4px 0; }
.toc > ul > li > a { font-weight:bold; font-size:11.5pt; }
.toc ul ul { list-style:none; padding-left:18px; }
.toc ul ul ul { padding-left:18px; font-size:9.5pt; }
.toc li { margin:2px 0; }
.toc a { text-decoration:none; color:#14507a; }
/* 目录右侧显示页码 */
.toc a::after { content: leader(". ") target-counter(attr(href), page);
                color:#888; }
.toc > div > p { display:none; }
h1.t { break-before:auto !important; }
.tocbox h2 { border-bottom:none; }
"""

html_doc = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>408 四科考点精讲</title>
<style>{CSS}</style>
</head>
<body>
<header class="title">
  <h1 class="t">408 四科考点精讲</h1>
  <div class="sub">以考研助手 AI-Passport 设备为贯穿案例 · 数据结构 / 计组 / 操作系统 / 计网</div>
</header>
<div class="tocbox">
  <h2>目录（点击可跳转）</h2>
  {toc}
</div>
<div class="content">
{body}
</div>
</body>
</html>"""

with open("/workspace/docs/_preview.html", "w", encoding="utf-8") as f:
    f.write(html_doc)

HTML(string=html_doc, base_url="/workspace/docs").write_pdf(OUT)
print("OK ->", OUT)