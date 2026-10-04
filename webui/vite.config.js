import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// base: './' —— 产物以 file:// 被 pywebview 加载，必须使用相对路径
export default defineConfig({
  base: './',
  plugins: [vue()],
  build: {
    outDir: 'dist',
    assetsDir: 'assets',
    chunkSizeWarningLimit: 2000,
  },
  server: {
    port: 5173,
  },
})