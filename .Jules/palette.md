## 2024-05-18 - Tooltip triggers on disabled UI elements
**Learning:** React/Ant Design tooltips applied to disabled components (like `<Button disabled>`) do not display because disabled HTML elements generally consume mouse events without triggering hover states.
**Action:** Always wrap the disabled element within a `<span>` and set `style={{ pointerEvents: 'none' }}` conditionally on the disabled element. Additionally, use `tabIndex={0}` on the wrapper to ensure keyboard accessibility.
