import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { fileURLToPath, URL } from 'node:url'

export default defineConfig({
  // 确保 WASM 文件被正确处理
  assetsInclude: ['**/*.wasm'],
  plugins: [vue()],
  resolve: {
    alias: { '@': fileURLToPath(new URL('./src', import.meta.url)) },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    rollupOptions: {
      external: [],
      output: {
        manualChunks: {}
      }
    }
  },
  optimizeDeps: {
    include: [],
    exclude: []
  },
  server: {
    port: 5173,
    proxy: {
      '/api': { target: 'http://127.0.0.1:8080', changeOrigin: true },
      '/zlm': { target: 'http://127.0.0.1:880', changeOrigin: true },
    },
  },
})
