# Palette UI Memory

## Frontend Stack
- The web UI is a Vite + React 18 + TypeScript + Ant Design 5 SPA under `web/`.
- Prefer existing Ant Design components and current interaction patterns over introducing new UI libraries or large custom styling systems.

## Disabled Controls And Tooltips
- Ant Design tooltips do not fire reliably on disabled interactive elements. Wrap the disabled control in a `<span style={{ display: 'inline-block' }}>`.
- Apply `tabIndex`, `role="button"`, and `aria-disabled` to the wrapper only while the inner control is actually disabled. This preserves accessibility without creating double-focus traps when the control is enabled.
- Use `style={{ pointerEvents: 'none' }}` on the disabled inner control when needed to preserve tooltip hover behavior.

## Accessible Actions
- Icon-only actions need an explicit `aria-label` in addition to any tooltip text.
- Keep labels, helper text, and error messages aligned with the current product tone and avoid relying on visual context alone.

## Async Interaction Rules
- Async UI actions should expose visible progress, typically with component `loading` states and `message.loading(...)` for longer operations.
- Wrap async handlers in `try/catch/finally`, show explicit success or error feedback, and roll back optimistic UI state on failure.
- While async work is in flight, disable or gate repeated user actions that would create races or duplicate requests.
## 2024-05-18 - Tooltip triggers on disabled UI elements
**Learning:** React/Ant Design tooltips applied to disabled components (like `<Button disabled>`) do not display because disabled HTML elements generally consume mouse events without triggering hover states.
**Action:** Always wrap the disabled element within a `<span>` and set `style={{ pointerEvents: 'none' }}` conditionally on the disabled element. Additionally, use `tabIndex={0}` on the wrapper to ensure keyboard accessibility.
## 2025-02-23 - Accessible Icon Buttons in Form Labels
**Learning:** When introducing icon-only actions (like a refresh button) next to Form labels or within constrained layouts, relying solely on visual icons degrades screen reader accessibility.
**Action:** Always combine a visual `Tooltip` with an explicit `aria-label` on the button itself. This satisfies both sighted users needing context and screen reader users navigating interactively.
## 2024-05-24 - Fix Double Focus on Conditional Icon Buttons
**Learning:** When using a `<span>` wrapper with `tabIndex={0}` to preserve `<Tooltip>` on a disabled `<Button>` in Ant Design, it creates a 'double focus' bug when the button is enabled (users tab to the span, then the button).
**Action:** Conditionally apply `tabIndex={0}`, `role='button'`, and `aria-disabled={true}` to the `<span>` wrapper *only* when the inner button is actually disabled (e.g., in a loading state). Always pair this with `style={{ display: 'inline-block' }}` to prevent layout shifts or invisible hit-areas.
## 2025-02-23 - Async Feedback for Select Components
**Learning:** React/Ant Design `<Select>` components that trigger asynchronous operations (like API calls on `onChange`) lack built-in `loading` prop support in the same way `<Button>` or `<Switch>` do. This can lead to users repeatedly changing selections before the first request finishes, causing race conditions or missing feedback.
**Action:** Wrap the `onChange` asynchronous call in a `try/catch/finally` block. Use `message.loading('...', 0)` to provide persistent feedback and hide it in the `finally` block. Conditionally disable the `<Select>` component using `disabled={isLoading}` during the async operation to prevent concurrent modifications.
## 2025-02-23 - Graceful Degradation for MJPEG Streams
**Learning:** Raw `<img>` elements pointing to continuous streams (like MJPEG) display a jarring broken image icon if the stream connection fails or the backend service is down. This provides no actionable context to the user.
**Action:** Always wrap volatile image streams and catch `onError` events to trigger a friendly fallback state (like an Ant Design `Empty` component) explaining the failure and offering a clear "Retry" action. Ensure image alternative text (`alt`) is descriptive ("摄像头实时预览" rather than "preview") for screen reader users when the image does render.
