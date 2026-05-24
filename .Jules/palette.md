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
