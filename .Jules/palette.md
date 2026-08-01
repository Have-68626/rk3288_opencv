## 2024-08-01 - Replace plain text empty states with Empty component
**Learning:** Plain text placeholders like `<Typography.Text>` for empty arrays or null configurations are visually inconsistent and lack the visual affordance of a proper empty state.
**Action:** Replace plain text placeholders for missing or unloaded data with Ant Design's `<Empty>` component (e.g., `<Empty image={Empty.PRESENTED_IMAGE_SIMPLE} description="..." />`) to maintain visual consistency and improve the interface's empty state design.
