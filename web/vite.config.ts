import { defineConfig, loadEnv } from 'vite'
import react from '@vitejs/plugin-react'
import istanbul from 'vite-plugin-istanbul'

// https://vite.dev/config/
export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), '')
  // 说明：
  // - Windows 本地服务（CivetWeb）会把仓库的 src/win/app/webroot 当作静态目录托管；
  // - 因此这里将构建产物直接输出到该目录，避免“再拷贝一份”导致版本错乱。
  // - base 使用 ./ 以兼容“非根路径/文件协议/被嵌入子目录”等场景（相对路径更稳）。
  const devProxyTarget = env.VITE_DEV_PROXY_TARGET || 'http://127.0.0.1:8080'

  return {
    base: './',
    plugins: [
      react(),
      process.env.CYPRESS_COVERAGE === '1'
        ? istanbul({
            include: ['src/**/*.ts', 'src/**/*.tsx'],
            exclude: ['**/*.d.ts'],
            extension: ['.ts', '.tsx'],
            cypress: true,
            requireEnv: false,
          })
        : undefined,
    ].filter(Boolean),
    server: {
      // 开发态通常是 Vite:5173 + 后端:8080 分离。
      // 通过代理避免 CORS（也更贴近生产同源：/api/v1/*）。
      proxy: {
        '/api': {
          target: devProxyTarget,
          changeOrigin: true,
        },
      },
    },
    build: {
      outDir: '../src/win/app/webroot',
      emptyOutDir: true,
    },
  }
})
