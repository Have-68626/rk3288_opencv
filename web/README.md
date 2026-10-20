<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-10-18
-->

﻿# Web SPA 前端



Windows 本地服务的浏览器 UI，基于 **React 18 + TypeScript 5 + Vite 8 + Ant Design 5**。

## 快速命令

```bash
# 安装依赖
pnpm install

# 开发模式（代理 /api → http://127.0.0.1:8080）
pnpm dev

# 构建（输出到 src/win/app/webroot/）
pnpm build

# 代码检查
pnpm lint

# 前端单元测试（Vitest）
pnpm test

# E2E须先准备服务器，普通执行和覆盖率验收前提见下文
pnpm e2e:run
```

## E2E与覆盖率前提

统一步骤见[构建与测试](../docs/guides/build-and-test.md#cypress-e2e与覆盖率)。Windows从仓库根执行 `pwsh -NoProfile -File scripts/run-web-e2e.ps1` 可运行完整驱动。手动流程须先启动127.0.0.1:5173的Vite，覆盖率变量在服务器启动前设置，再运行带环境隔离的启动器及单独覆盖率检查。

`pnpm e2e:run` 不启动服务器；`pnpm preview` 不是当前覆盖率插桩服务器。API拦截测试通过也不代表真实后端验收通过。

## 构建输出

- `pnpm build` 产出写入 `../src/win/app/webroot/`
- 这些文件被 git 跟踪（CMake 构建时会复制到 exe 同级的 `webroot/`）
- 开发代理目标可通过 `VITE_DEV_PROXY_TARGET` 环境变量覆盖

## 技术栈

- Vite 8 + @vitejs/plugin-react
- React Router v6
- Ant Design 5 + @ant-design/icons
- Cypress 13（E2E 测试）
- Vitest 4（单元测试）
- ESLint 9 + typescript-eslint
