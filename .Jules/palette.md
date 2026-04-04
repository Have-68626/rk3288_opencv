## 2025-04-04 - Screen Reader Noise from Decorative Icons
**Learning:** Purely decorative trailing icons within interactive list items (e.g., info or chevron icons where the row is clickable) cause unnecessary screen reader noise and trigger missing `contentDescription` lint warnings if not handled.
**Action:** Mark these purely decorative UI elements with `android:importantForAccessibility="no"` to improve the accessibility experience.
