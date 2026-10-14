<script setup lang="ts">
import { ref, onMounted, nextTick, computed } from 'vue';
import { Bridge } from '@/utils/bridge';
import { useI18n } from 'vue-i18n';

const { t } = useI18n();
const logContent = ref('');
const loading = ref(false);
const terminalBody = ref<HTMLElement | null>(null);

const fetchLog = async () => {
  loading.value = true;
  try {
    const text = await Bridge.getDaemonLog();
    logContent.value = text || '';
    
    // 自动滚动到底部
    await nextTick();
    if (terminalBody.value) {
      terminalBody.value.scrollTop = terminalBody.value.scrollHeight;
    }
  } finally {
    loading.value = false;
  }
};

// 使用正则对日志内容进行高亮解析
const formattedLog = computed(() => {
  if (!logContent.value) return `<div class="log-empty">${t('log_empty')}</div>`;
  
  return logContent.value.split('\n').map(line => {
    // 过滤掉空行
    if (!line.trim()) return '';

    // 1. 高亮时间戳 [2026-02-23 02:31:07]
    let html = line.replace(/\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\]/g, match => `<span class="log-time">${match}</span>`);
    
    // 2. 高亮日志等级 [INFO], [WARN], [ERROR]
    html = html.replace(/\[INFO\]/g, `<span class="log-info">[INFO]</span>`);
    html = html.replace(/\[WARN\]/g, `<span class="log-warn">[WARN]</span>`);
    html = html.replace(/\[ERROR\]/g, `<span class="log-error">[ERROR]</span>`);
    
    // 3. 高亮模块标签 [aegis::xxx] / [Scheduler] 等
    html = html.replace(/\[(aegis[^\]]*|yumi[^\]]*|Scheduler|AppDetect|Screen|Boot)\]/g, match => `<span class="log-tag">${match}</span>`);

    return `<div class="log-line">${html}</div>`;
  }).join('');
});

onMounted(() => {
  fetchLog();
});
</script>

<template>
  <div class="log-viewer">
    <van-nav-bar
      :title="t('view_log')"
      left-arrow
      @click-left="$router.back()"
      fixed
      placeholder
      z-index="100"
    >
      <template #right>
        <van-icon name="replay" size="18" @click="fetchLog" />
      </template>
    </van-nav-bar>

    <van-loading v-if="loading && !logContent" class="loading-center" vertical>{{ t('loading') }}</van-loading>

    <div v-else class="log-body" ref="terminalBody">
      <div class="log-container" v-html="formattedLog"></div>
    </div>
  </div>
</template>

<style scoped>
.log-viewer {
  min-height: 100vh;
  /* Aegis: 纯白底、无卡片无边框 */
  background-color: #ffffff;
  padding-bottom: 20px;
}

.loading-center {
  padding-top: 100px;
}

/* 日志正文区：贴边铺白，无圆角无边框 */
.log-body {
  padding: 12px 14px;
  overflow-y: auto;
  height: calc(100vh - 56px);
  box-sizing: border-box;
  scrollbar-width: thin;
  scrollbar-color: #c8c9cc #ffffff;
}
.log-body::-webkit-scrollbar {
  width: 5px;
}
.log-body::-webkit-scrollbar-thumb {
  background-color: #dcdfe6;
  border-radius: 10px;
}

.log-container {
  font-family: 'JetBrains Mono', 'Fira Code', 'Courier New', Consolas, monospace;
  font-size: 12.5px;
  line-height: 1.7;
  color: #1f2329; /* 黑字 */
  word-wrap: break-word;
  white-space: pre-wrap;
}

/* === 语法高亮：仅标签着色，保证白底黑字清晰可读 === */
:deep(.log-empty) { color: #8a8f99; font-style: italic; }
:deep(.log-line) { margin-bottom: 1px; }
:deep(.log-time) { color: #8a8f99; }
:deep(.log-info) { color: #1677ff; font-weight: bold; }
:deep(.log-warn) { color: #d48806; font-weight: bold; }
:deep(.log-error) { color: #e02020; font-weight: bold; }
:deep(.log-tag) { color: #722ed1; }
</style>
