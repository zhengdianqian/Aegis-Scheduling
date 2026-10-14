<script setup lang="ts">
import { ref, onMounted } from 'vue';
import { useI18n } from 'vue-i18n';
import { exec } from '@/kernelsu';

const { t } = useI18n();
const MODDIR = '/data/adb/modules/aegis_power_matrix';

const modes = [
  {
    key: 'official',
    icon: 'underway-o',
    gradient: 'linear-gradient(135deg, #36c5a5, #0f9d88)',
    desc: () => t('th_official_desc'),
  },
  {
    key: 'jizhi',
    icon: 'fire-o',
    gradient: 'linear-gradient(135deg, #ff9a3d, #f4511e)',
    desc: () => t('th_jizhi_desc'),
  },
  {
    key: 'sangxin',
    icon: 'fire',
    gradient: 'linear-gradient(135deg, #ff5b6a, #c8102e)',
    desc: () => t('th_sangxin_desc'),
  },
];

const current = ref('official');
const busy = ref(false);
const hint = ref('');

async function sh(cmd: string): Promise<string> {
  try {
    const r: any = await exec(cmd);
    return (r?.stdout || '').trim();
  } catch {
    return '';
  }
}

async function applyMode(key: string) {
  if (busy.value || key === current.value) return;
  busy.value = true;
  const prev = current.value;
  current.value = key;
  const rc = await sh(`echo "${key}" > ${MODDIR}/thermal_mode ; ${MODDIR}/thermal_manager ${MODDIR} apply ${key} &`);
  // 简单确认：文件已写入
  setTimeout(async () => {
    busy.value = false;
    const saved = await sh(`cat ${MODDIR}/thermal_mode 2>/dev/null`);
    if (saved === key) {
      hint.value = (t('th_applied', { name: t('th_' + key) as string }) as string);
    } else {
      current.value = prev;
      hint.value = (t('th_apply_failed') as string);
    }
    setTimeout(() => (hint.value = ''), 2500);
  }, 1800);
  void rc;
}

onMounted(async () => {
  const m = await sh(`cat ${MODDIR}/thermal_mode 2>/dev/null`);
  if (m === 'jizhi' || m === 'sangxin' || m === 'official') current.value = m;
});
</script>

<template>
  <div class="thermal-page">
    <van-nav-bar
      :title="t('menu_thermal')"
      left-arrow
      @click-left="$router.back()"
      fixed
      placeholder
    />

    <div class="tip-banner">
      <van-icon name="info-o" size="16" color="#2196F3" />
      <span>{{ t('thermal_tip') }}</span>
    </div>

    <div class="mode-list">
      <div
        v-for="m in modes"
        :key="m.key"
        class="mode-card"
        :class="{ active: current === m.key }"
        @click="applyMode(m.key)"
      >
        <div class="mode-icon" :style="{ background: m.gradient }">
          <van-icon :name="m.icon" size="32" color="#fff" />
        </div>
        <div class="mode-body">
          <div class="mode-name">{{ t('th_' + m.key) }}</div>
          <div class="mode-desc">{{ m.desc() }}</div>
        </div>
        <van-icon
          :name="current === m.key ? 'success' : 'circle'"
          size="22"
          :color="current === m.key ? '#2196F3' : '#dcdee0'"
        />
      </div>
    </div>

    <transition name="fade">
      <div v-if="hint" class="th-toast">{{ hint }}</div>
    </transition>
  </div>
</template>

<style scoped>
.thermal-page {
  min-height: 100vh;
  background: #f7f8fa;
  padding-bottom: 40px;
}

.tip-banner {
  margin: 14px 16px 4px;
  display: flex;
  align-items: center;
  gap: 8px;
  background: #ecf5ff;
  color: #4a7ab8;
  font-size: 12.5px;
  line-height: 1.5;
  padding: 10px 14px;
  border-radius: 12px;
}

.mode-list {
  padding: 12px 16px;
  display: flex;
  flex-direction: column;
  gap: 14px;
}

.mode-card {
  display: flex;
  align-items: center;
  gap: 16px;
  background: #fff;
  border-radius: 18px;
  padding: 20px 18px;
  box-shadow: 0 2px 10px rgba(0, 0, 0, 0.05);
  border: 2px solid transparent;
  transition: transform 0.15s ease, border-color 0.2s ease, box-shadow 0.2s ease;
  cursor: pointer;
}
.mode-card:active { transform: scale(0.985); }
.mode-card.active {
  border-color: #2196f3;
  box-shadow: 0 6px 18px rgba(33, 150, 243, 0.18);
}

.mode-icon {
  width: 62px;
  height: 62px;
  border-radius: 18px;
  display: flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
  box-shadow: 0 5px 12px rgba(0, 0, 0, 0.15);
}

.mode-body { flex: 1; min-width: 0; }
.mode-name {
  font-size: 19px;
  font-weight: 700;
  color: #1f2329;
  margin-bottom: 6px;
}
.mode-desc {
  font-size: 13px;
  color: #969799;
  line-height: 1.5;
}

.th-toast {
  position: fixed;
  left: 50%;
  bottom: 60px;
  transform: translateX(-50%);
  background: rgba(34, 34, 34, 0.88);
  color: #fff;
  padding: 10px 18px;
  border-radius: 22px;
  font-size: 13px;
  z-index: 999;
}
.fade-enter-active,
.fade-leave-active {
  transition: opacity 0.25s;
}
.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}
</style>
