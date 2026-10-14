<script setup lang="ts">
import { onMounted, computed, ref } from 'vue';
import { useRouter } from 'vue-router';
import { useSchedulerStore } from '@/stores/scheduler';
import { useI18n } from 'vue-i18n';
import { exec } from '@/kernelsu';
import shieldLogo from '@/assets/shield.png';

const store = useSchedulerStore();
const { t } = useI18n();
const router = useRouter();

// 模式列表
const modes = computed(() => [
  { key: 'powersave', name: t('mode_powersave'), desc: t('desc_powersave'), icon: 'shield-o', color: '#4CAF50' },
  { key: 'balance', name: t('mode_balance'), desc: t('desc_balance'), icon: 'balance-o', color: '#2196F3' },
  { key: 'performance', name: t('mode_performance'), desc: t('desc_performance'), icon: 'fire', color: '#FF9800' },
  { key: 'fast', name: t('mode_fast'), desc: t('desc_fast'), icon: 'upgrade', color: '#F44336' },
]);

const MODDIR = '/data/adb/modules/aegis_power_matrix';
const thermalLabel = ref('');
const chargeOn = ref<boolean | null>(null);

async function sh(cmd: string): Promise<string> {
  try {
    const r: any = await exec(cmd);
    return (r?.stdout || '').trim();
  } catch {
    return '';
  }
}

async function refreshCardStates() {
  const m = await sh(`cat ${MODDIR}/thermal_mode 2>/dev/null`);
  thermalLabel.value = m === 'jizhi' ? (t('th_jizhi') as string)
    : m === 'sangxin' ? (t('th_sangxin') as string)
    : (t('th_official') as string);
  chargeOn.value = (await sh(`[ -f ${MODDIR}/charge_guard/data/off ] && echo off || echo on`)) !== 'off';
}

onMounted(() => {
  store.initData();
  refreshCardStates();
});

const handleModeSelect = async (modeKey: string) => {
  await store.switchMode(modeKey);
};

const goCharge = () => router.push('/charge');
const goThermal = () => router.push('/thermal');
</script>

<template>
  <div class="home-container">

    <!-- 大气头部：盾牌 + 居中 Aegis -->
    <div class="hero">
      <img class="hero-logo" :src="shieldLogo" alt="Aegis" />
      <h1 class="hero-title">Aegis</h1>
      <p class="hero-sub">eBPF 负载调频 · 帧感知调度</p>
    </div>

    <!-- 守护 / 当前模式 -->
    <div class="header-cards">
      <div class="status-card daemon-card" :style="{ background: store.isDaemonRunning ? '#10b981' : '#9ca3af' }">
        <van-icon :name="store.isDaemonRunning ? 'checked' : 'warning-o'" size="30" />
        <div class="info">
          <h2>Aegis</h2>
          <p>{{ store.isDaemonRunning ? t('daemon_running') : t('daemon_stopped') }}</p>
        </div>
      </div>

      <div class="status-card mode-card" :style="{ background: modes.find(m => m.key === store.currentMode)?.color || '#2196F3' }">
        <van-icon :name="modes.find(m => m.key === store.currentMode)?.icon || 'balance-o'" size="30" />
        <div class="info">
          <h2>{{ modes.find(m => m.key === store.currentMode)?.name || t('unknown_mode') }}</h2>
          <p>{{ t('current_status') }}</p>
        </div>
      </div>
    </div>

    <div class="section-title">{{ t('global_mode') }}</div>
    <van-grid :column-num="2" :gutter="12" :border="false" class="mode-grid">
      <van-grid-item v-for="mode in modes" :key="mode.key">
        <div class="mode-card-content"
             :class="{ 'is-active': store.currentMode === mode.key }"
             :style="store.currentMode === mode.key ? { backgroundColor: mode.color } : {}"
             @click="handleModeSelect(mode.key)">
          <van-icon :name="mode.icon" size="26" :color="store.currentMode === mode.key ? '#fff' : mode.color" />
          <span class="mode-name" :style="{ color: store.currentMode === mode.key ? '#fff' : '#323233' }">{{ mode.name }}</span>
          <span class="mode-desc" :style="{ color: store.currentMode === mode.key ? 'rgba(255,255,255,0.8)' : '#969799' }">{{ mode.desc }}</span>
        </div>
      </van-grid-item>
    </van-grid>

    <div class="section-title">{{ t('functions') }}</div>
    <van-grid :column-num="3" :gutter="12" :border="false" class="mini-grid">
      <van-grid-item icon="apps-o" :text="t('app_management')" to="/apps" />
      <van-grid-item icon="setting-o" :text="t('detailed_config')" to="/config" />
      <van-grid-item icon="notes-o" :text="t('view_log')" to="/log" />
    </van-grid>

    <!-- 充电 / 温控：独立大入口 -->
    <div class="big-entry charge-entry" @click="goCharge">
      <div class="big-icon charge-icon">
        <svg viewBox="0 0 24 24" width="30" height="30" fill="#fff" aria-hidden="true">
          <path d="M13 2 4.6 13.6H11l-.9 8.4 8.3-11.7H12.1L13 2z" />
        </svg>
      </div>
      <div class="big-text">
        <div class="big-title">{{ t('menu_charge') }}</div>
        <div class="big-sub">{{ chargeOn === null ? '—' : (chargeOn ? t('cg_charging_on') : t('cg_off_hint')) }} · {{ t('charge_sub') }}</div>
      </div>
      <van-icon name="arrow" size="18" color="#c8c9cc" />
    </div>

    <div class="big-entry thermal-entry" @click="goThermal">
      <div class="big-icon thermal-icon">
        <van-icon name="fire-o" size="30" color="#fff" />
      </div>
      <div class="big-text">
        <div class="big-title">{{ t('menu_thermal') }}</div>
        <div class="big-sub">{{ t('thermal_sub') }} · {{ thermalLabel || '—' }}</div>
      </div>
      <van-icon name="arrow" size="18" color="#c8c9cc" />
    </div>
  </div>
</template>

<style scoped>
.home-container {
  padding-bottom: 40px;
  background-color: #f7f8fa;
  min-height: 100vh;
}

/* 大气头部 */
.hero {
  margin: 0 0 4px;
  padding: 34px 20px 30px;
  border-radius: 0 0 24px 24px;
  background: linear-gradient(140deg, #1e6bff 0%, #0a3fb0 60%, #072d80 100%);
  color: #fff;
  display: flex;
  flex-direction: column;
  align-items: center;
  box-shadow: 0 10px 28px rgba(16, 86, 220, 0.28);
}
.hero-logo {
  width: 92px;
  height: 92px;
  filter: drop-shadow(0 6px 14px rgba(0,0,0,0.25));
}
.hero-title {
  margin: 12px 0 0;
  font-size: 40px;
  font-weight: 800;
  letter-spacing: 6px;
  text-indent: 6px;
  line-height: 1.1;
}
.hero-sub {
  margin: 8px 0 0;
  font-size: 13px;
  letter-spacing: 2px;
  color: rgba(255,255,255,0.82);
}

/* 顶部双卡片 */
.header-cards { display: flex; gap: 12px; margin: 16px; }
.status-card {
  flex: 1; padding: 16px 8px; border-radius: 16px; color: white;
  display: flex; flex-direction: column; align-items: center; justify-content: center;
  box-shadow: 0 6px 16px rgba(0,0,0,0.12); transition: all 0.3s ease; text-align: center;
}
.status-card .info { margin-top: 8px; }
.status-card h2 { margin: 0; font-size: 16px; font-weight: 600; }
.status-card p { margin: 4px 0 0; opacity: 0.9; font-size: 12px; }

:deep(.van-grid-item__content) { padding: 0 !important; background-color: transparent !important; }

/* 模式卡片 */
.mode-card-content {
  width: 100%; height: 96px; display: flex; flex-direction: column;
  align-items: center; justify-content: center; background-color: #fff;
  border-radius: 12px; box-shadow: 0 2px 8px rgba(0,0,0,0.04);
  transition: all 0.3s cubic-bezier(0.25, 0.8, 0.25, 1); box-sizing: border-box;
  cursor: pointer;
}
.mode-card-content:active { transform: scale(0.95); opacity: 0.9; }
.mode-card-content.is-active { box-shadow: 0 6px 16px rgba(0,0,0,0.15); transform: translateY(-2px); }
.mode-name { margin-top: 8px; font-size: 14px; font-weight: 600; }
.mode-desc { margin-top: 4px; font-size: 11px; }

/* 三个小功能入口 */
.mini-grid :deep(.van-grid-item__content) {
  background: #fff !important;
  border-radius: 12px;
  padding: 14px 4px !important;
  box-shadow: 0 2px 8px rgba(0,0,0,0.04);
}
.mini-grid :deep(.van-grid-item__text) { font-size: 13px; color: #323233; }

.section-title {
  margin: 20px 16px 10px; font-size: 14px; color: #969799; font-weight: 500;
}

/* 充电 / 温控 大入口 */
.big-entry {
  margin: 14px 16px 0;
  background: #fff;
  border-radius: 16px;
  padding: 18px 16px;
  display: flex;
  align-items: center;
  gap: 14px;
  box-shadow: 0 2px 10px rgba(0,0,0,0.05);
  transition: transform 0.15s ease;
  cursor: pointer;
}
.big-entry:active { transform: scale(0.98); }
.big-icon {
  width: 54px; height: 54px; border-radius: 14px;
  display: flex; align-items: center; justify-content: center;
  flex-shrink: 0;
  box-shadow: 0 4px 10px rgba(0,0,0,0.15);
}
.charge-icon { background: linear-gradient(135deg, #22c16b, #0f9d58); }
.thermal-icon { background: linear-gradient(135deg, #ff8a3d, #f44336); }
.big-text { flex: 1; min-width: 0; }
.big-title { font-size: 17px; font-weight: 700; color: #1f2329; }
.big-sub { margin-top: 4px; font-size: 12px; color: #969799; }
</style>
