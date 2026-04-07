## 2024-04-05 - Add Popconfirm for destructive action
**Learning:** Destructive actions without confirmation are a critical UX and data loss risk. Using standard library components like Ant Design's `Popconfirm` provides immediate, localized safety without breaking the layout.
**Action:** When creating or identifying buttons for destructive actions (e.g., delete, clear), verify they require confirmation via a dialog or inline pop-up before execution.
