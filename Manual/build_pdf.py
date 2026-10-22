"""Renders manual.html to TapeFour-Manual.pdf with the pre-installed Chromium (Playwright)."""
import pathlib
from playwright.sync_api import sync_playwright

here = pathlib.Path(__file__).resolve().parent
footer = ('<div style="width:100%;font:8px Inter,sans-serif;color:#6b665c;padding:0 17mm;display:flex;justify-content:space-between">'
          '<span>TAPE·FOUR OWNER\'S MANUAL</span><span class="pageNumber"></span></div>')

with sync_playwright() as p:
    browser = p.chromium.launch()
    page = browser.new_page()
    page.goto((here / "manual.html").as_uri())
    page.wait_for_load_state("networkidle")
    page.evaluate("document.fonts.ready")
    page.pdf(path=str(here / "TapeFour-Manual.pdf"), format="A4", print_background=True,
             prefer_css_page_size=True, display_header_footer=True,
             header_template="<div></div>", footer_template=footer)
    browser.close()
print("written", here / "TapeFour-Manual.pdf")
