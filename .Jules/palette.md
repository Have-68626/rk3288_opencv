## 2025-04-04 - Screen Reader Noise from Decorative Icons
**Learning:** Purely decorative trailing icons within interactive list items (e.g., info or chevron icons where the row is clickable) cause unnecessary screen reader noise and trigger missing `contentDescription` lint warnings if not handled.
**Action:** Mark these purely decorative UI elements with `android:importantForAccessibility="no"` to improve the accessibility experience.

## 2025-04-03 - Marking List Item Indicators as Decorative
**Learning:** In Android RecyclerView items, trailing icons (like "info" or chevron arrows) used purely as visual indicators for interactive list items often trigger "Missing contentDescription" accessibility warnings in Lint. Since the list item itself handles the click action, these icons do not need independent descriptions, which would just add noise for screen reader users.
**Action:** Use `android:importantForAccessibility="no"` on these decorative `ImageView` elements to hide them from screen readers instead of providing a redundant `contentDescription`.
