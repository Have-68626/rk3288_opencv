import os
from playwright.sync_api import sync_playwright

def get_artifact_dirs():
    base_dir = os.environ.get(
        "RK_VERIFY_ARTIFACTS_DIR",
        os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "tests", "reports", "playwright"))
    )
    screenshot_dir = os.path.join(base_dir, "screenshots")
    video_dir = os.path.join(base_dir, "videos")
    os.makedirs(screenshot_dir, exist_ok=True)
    os.makedirs(video_dir, exist_ok=True)
    return screenshot_dir, video_dir

def run_cuj(page, screenshot_dir):
    page.goto("http://localhost:5173/#/")
    page.wait_for_timeout(1000)

    # Click the "继续进入" button on the SplashPage
    page.get_by_role("button", name="继续进入").click()
    page.wait_for_timeout(1000)

    # Navigate to Settings
    page.get_by_role("menuitem", name="设置").click()
    page.wait_for_timeout(1000)

    # Click on server tab
    page.get_by_role("tab", name="后端（API）").click()
    page.wait_for_timeout(1000)

    # Scroll down to show the button
    page.evaluate("window.scrollTo(0, document.body.scrollHeight)")
    page.wait_for_timeout(1000)

    # Take screenshot at the key moment (showing the Popconfirm and the loading state of the button)
    screenshot_path = os.path.join(screenshot_dir, "verification2.png")
    page.screenshot(path=screenshot_path)
    page.wait_for_timeout(1000)

if __name__ == "__main__":
    screenshot_dir, video_dir = get_artifact_dirs()
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        context = browser.new_context(
            record_video_dir=video_dir
        )
        page = context.new_page()
        try:
            run_cuj(page, screenshot_dir)
        finally:
            context.close()
            browser.close()
