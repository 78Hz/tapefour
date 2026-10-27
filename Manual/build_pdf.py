"""Renders the manuals to PDF with the pre-installed Chromium (Playwright):
manual.html -> TapeFour-Manual.pdf (the TF-4) and pocket.html -> TF-P1-Pocket-Guide.pdf (the player).
Pass a name (tapefour, pocket) to build just one."""
import pathlib, sys
from playwright.sync_api import sync_playwright

here = pathlib.Path(__file__).resolve().parent
books = {
    "tapefour": ("manual.html", "TapeFour-Manual.pdf", "TAPE·FOUR OWNER'S MANUAL"),
    "pocket": ("pocket.html", "TF-P1-Pocket-Guide.pdf", "TF-P1 POCKET OWNER'S GUIDE"),
}
wanted = sys.argv[1:] or list(books)

def footer(title):
    return ('<div style="width:100%;font:8px Inter,sans-serif;color:#6b665c;padding:0 17mm;display:flex;justify-content:space-between">'
            '<span>' + title.replace("'", "&#39;") + '</span><span class="pageNumber"></span></div>')

with sync_playwright() as p:
    browser = p.chromium.launch()
    for name in wanted:
        src, pdf, title = books[name]
        page = browser.new_page()
        page.goto((here / src).as_uri())
        page.wait_for_load_state("networkidle")
        page.evaluate("document.fonts.ready")
        page.pdf(path=str(here / pdf), format="A4", print_background=True,
                 prefer_css_page_size=True, display_header_footer=True,
                 header_template="<div></div>", footer_template=footer(title))
        page.close()
        print("written", here / pdf)
    browser.close()
