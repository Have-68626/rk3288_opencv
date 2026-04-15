from playwright.sync_api import sync_playwright

def run_cuj(page):
    page.goto("http://localhost:5173/#/")
    page.wait_for_timeout(1000)

    # Click the splash screen continue button if present
    try:
        page.get_by_role("button", name="继续进入").click()
        page.wait_for_timeout(1000)
    except:
        pass

    # We should now be on the home page.
    # The empty state text should say "尚未加载。可点击右上角“刷新后端设置”，或检查后端服务是否已启动。"
    # and there should be a "刷新后端设置" button in the card.

    page.screenshot(path="tests/reports/verification/screenshots/verification.png")
    page.wait_for_timeout(1500)

if __name__ == "__main__":
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        context = browser.new_context(
            record_video_dir="tests/reports/verification/videos",
            viewport={'width': 1280, 'height': 800}
        )
        page = context.new_page()
        try:
            run_cuj(page)
        finally:
            context.close()
            browser.close()