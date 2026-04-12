from playwright.sync_api import sync_playwright

def run_cuj(page):
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
    page.screenshot(path="/home/jules/verification/screenshots/verification2.png")
    page.wait_for_timeout(1000)

if __name__ == "__main__":
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True)
        context = browser.new_context(
            record_video_dir="/home/jules/verification/videos"
        )
        page = context.new_page()
        try:
            run_cuj(page)
        finally:
            context.close()
            browser.close()
