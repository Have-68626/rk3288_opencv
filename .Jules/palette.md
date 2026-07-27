## 2024-07-27 - Empty States with Ant Design
**Learning:** Plain text placeholders for empty lists reduce visual hierarchy and look broken or forgotten compared to the rest of the Ant Design component library.
**Action:** Always replace plain text empty states (like `<Typography.Text>`) with Ant Design's `<Empty>` component (e.g., `<Empty image={Empty.PRESENTED_IMAGE_SIMPLE} description="..." />`) to maintain visual consistency and improve the interface's empty state design.
