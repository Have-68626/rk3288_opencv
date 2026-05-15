## 2024-05-18 - Tooltip triggers on disabled UI elements
**Learning:** React/Ant Design tooltips applied to disabled components (like `<Button disabled>`) do not display because disabled HTML elements generally consume mouse events without triggering hover states.
**Action:** Always wrap the disabled element within a `<span>` and set `style={{ pointerEvents: 'none' }}` conditionally on the disabled element. Additionally, use `tabIndex={0}` on the wrapper to ensure keyboard accessibility.
## 2025-02-23 - Accessible Icon Buttons in Form Labels
**Learning:** When introducing icon-only actions (like a refresh button) next to Form labels or within constrained layouts, relying solely on visual icons degrades screen reader accessibility.
**Action:** Always combine a visual `Tooltip` with an explicit `aria-label` on the button itself. This satisfies both sighted users needing context and screen reader users navigating interactively.
