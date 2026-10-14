import { fileURLToPath, URL } from 'node:url'
import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import Components from 'unplugin-vue-components/vite'
import { VantResolver } from '@vant/auto-import-resolver'
import { viteSingleFile } from 'vite-plugin-singlefile'

// 单文件构建：所有 JS/CSS/图片全部内联进 index.html，webroot 只产出一个文件
export default defineConfig({
  plugins: [
    vue(),
    // 自动导入 Vant 组件
    Components({
      resolvers: [VantResolver()],
    }),
    viteSingleFile(),
  ],
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url))
    }
  },
  // 强制打包为相对路径，确保在 WebUI 环境下资源能加载
  base: './',
  build: {
    assetsInlineLimit: 100000000,
    cssCodeSplit: false,
  }
})
