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
