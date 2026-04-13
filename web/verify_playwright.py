import os
from playwright.sync_api import sync_playwright

def run_cuj(page, screenshots_dir):
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
    screenshot_path = os.path.join(screenshots_dir, "verification2.png")
    page.screenshot(path=screenshot_path)
    page.wait_for_timeout(1000)

if __name__ == "__main__":
    # Determine the artifacts directory
    # 1. Check for RK_VERIFY_ARTIFACTS_DIR environment variable
    # 2. Fall back to {repo_root}/tests/reports/verification
    artifacts_dir = os.environ.get("RK_VERIFY_ARTIFACTS_DIR")
    if not artifacts_dir:
        # __file__ is web/verify_playwright.py, so its grandparent is repo root
        repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
        artifacts_dir = os.path.join(repo_root, "tests", "reports", "verification")

    screenshots_dir = os.path.join(artifacts_dir, "screenshots")
    videos_dir = os.path.join(artifacts_dir, "videos")

    # Automatically create output directories
    os.makedirs(screenshots_dir, exist_ok=True)
    os.makedirs(videos_dir, exist_ok=True)

    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        context = browser.new_context(
            record_video_dir=videos_dir
        )
        page = context.new_page()
        try:
            run_cuj(page, screenshots_dir)
        finally:
            context.close()
            browser.close()
