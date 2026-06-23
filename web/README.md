# Web SPA 前端

**更新日期**: 2026-06-23


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

# E2E 测试
pnpm e2e:run
pnpm e2e:run:coverage   # 带覆盖率
```

## 构建输出

- `pnpm build` 产出写入 `../src/win/app/webroot/`
- 这些文件被 git 跟踪（CMake 构建时会复制到 exe 同级的 `webroot/`）
- 开发代理目标可通过 `VITE_DEV_PROXY_TARGET` 环境变量覆盖

## 技术栈

- Vite 8 + @vitejs/plugin-react
- React Router v6
- Ant Design 5 + @ant-design/icons
- Cypress 13（E2E 测试）
- ESLint 9 + typescript-eslint
