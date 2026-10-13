# graphify
- **graphify** (`.claude/skills/graphify/SKILL.md`) - any input to knowledge graph. Trigger: `/graphify`
When the user types `/graphify`, use the installed graphify skill or instructions before doing anything else.

# 项目持久记忆（所有会话必须读取）
本项目的跨会话记忆位于 `C:\Users\19842\.claude\projects\D--19842-Documents-GitHub-rk3288-opencv\memory\`，`MEMORY.md` 为索引。
本文件随项目加载（含子目录会话），每次会话先读取 `MEMORY.md` 索引；涉及用户约定/项目经验/流程时，引用对应记忆文件（工作规则、安全红线、代码原则、Git 约定、SOP 与反思、任务审计循环、graphify 设置等）。子代理涉及上述领域时也须带上。
