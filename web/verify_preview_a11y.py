from playwright.sync_api import sync_playwright
import os
import glob

def run_cuj(page):
    page.goto("http://localhost:5173/#/")
    page.wait_for_timeout(1500)

    # Click 继续进入 if SplashPage is active
    try:
        page.get_by_role("button", name="继续进入").click(timeout=3000)
        page.wait_for_timeout(1000)
    except Exception:
        pass

    page.goto("http://localhost:5173/#/preview")
    page.wait_for_timeout(1500)

    # Click 翻转 X using the text/label
    page.get_by_text("翻转 X").click()
    page.wait_for_timeout(1000)

    # Click 翻转 Y using the text/label
    page.get_by_text("翻转 Y").click()
    page.wait_for_timeout(1000)

    # Click 注册 personId input
    page.locator("#preview-person-id").fill("alice")
    page.wait_for_timeout(1000)

    os.makedirs("/home/jules/verification/screenshots", exist_ok=True)
    page.screenshot(path="/home/jules/verification/screenshots/verification.png")
    page.wait_for_timeout(1000)

if __name__ == "__main__":
    os.makedirs("/home/jules/verification/videos", exist_ok=True)
    # clean up old videos
    for f in glob.glob("/home/jules/verification/videos/*.webm"):
        os.remove(f)

    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        context = browser.new_context(
            record_video_dir="/home/jules/verification/videos",
            viewport={"width": 1280, "height": 800}
        )
        page = context.new_page()
        try:
            run_cuj(page)
        finally:
            context.close()
            browser.close()
