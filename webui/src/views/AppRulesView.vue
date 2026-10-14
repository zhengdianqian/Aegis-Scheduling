<script setup lang="ts">
import { ref, onMounted, computed } from 'vue';
import { useI18n } from 'vue-i18n';
import { Bridge } from '@/utils/bridge';
import { getPackagesInfo, toast } from '@/kernelsu';
import { useSchedulerStore } from '@/stores/scheduler';

const { t } = useI18n();
const store = useSchedulerStore();

// FAS 全局开关（写入 rules.yaml 的 fas_enabled，守护进程热加载）
const fasEnabled = ref(true);
const fasLoading = ref(false);

const onFasToggle = async (v: boolean) => {
  fasLoading.value = true;
  const prev = fasEnabled.value;
  fasEnabled.value = v;
  try {
    const rules = await Bridge.getRulesConfig();
    rules.fas_enabled = v;
    await Bridge.saveRulesConfig(rules);
    toast(t('fas_switch_saved') as string);
  } catch (e) {
    fasEnabled.value = prev;
    toast(t('save_failed') as string);
  } finally {
    fasLoading.value = false;
  }
};

// pkg → appLabel 映射
const appLabelMap = ref<Record<string, string>>({});
const apps = ref<string[]>([]);
const searchText = ref('');
const showActionSheet = ref(false);
const selectedPkg = ref('');

const actions = computed(() => [
  { name: t('mode_powersave'), subname: t('desc_powersave'), color: '#4CAF50', modeKey: 'powersave' },
  { name: t('mode_balance'), subname: t('desc_balance'), color: '#2196F3', modeKey: 'balance' },
  { name: t('mode_performance'), subname: t('desc_performance'), color: '#FF9800', modeKey: 'performance' },
  { name: t('mode_fast'), subname: t('desc_fast'), color: '#F44336', modeKey: 'fast' },
  { name: t('mode_fas'), subname: t('desc_fas'), color: '#E91E63', modeKey: 'fas' },
  { name: t('delete_rule'), color: '#FF0000', isDelete: true }
]);

const modeLabel = (modeKey: string) => {
  switch (modeKey) {
    case 'powersave': return t('mode_powersave');
    case 'balance': return t('mode_balance');
    case 'performance': return t('mode_performance');
    case 'fast': return t('mode_fast');
    case 'fas': return t('mode_fas');
    default: return modeKey;
  }
};

onMounted(async () => {
  const packages = await Bridge.getInstalledApps();
  apps.value = packages;

  // 批量获取应用信息，建立 pkg → label 映射
  try {
    const infos = getPackagesInfo(packages);
    infos.forEach(info => {
      appLabelMap.value[info.packageName] = info.appLabel;
    });
  } catch (e) {
    // 获取失败时降级显示包名，不影响主流程
  }

  await store.initData();

  // 读取 FAS 全局开关当前状态
  try {
    const rules = await Bridge.getRulesConfig();
    fasEnabled.value = rules.fas_enabled !== false;
  } catch (e) {
    // 读取失败保持默认，不影响应用列表
  }
});

// 用应用名或包名都能搜到
const filteredApps = computed(() => {
  const q = searchText.value.toLowerCase();
  if (!q) return apps.value;
  return apps.value.filter(pkg =>
    pkg.toLowerCase().includes(q) ||
    (appLabelMap.value[pkg] || '').toLowerCase().includes(q)
  );
});

// 优先显示应用名，缺失时降级为包名
const getLabel = (pkg: string) => appLabelMap.value[pkg] || pkg;

const openMenu = (pkg: string) => {
  selectedPkg.value = pkg;
  showActionSheet.value = true;
};

const onSelectAction = async (item: any) => {
  showActionSheet.value = false;
  if (item.isDelete) {
    delete store.appRules[selectedPkg.value];
    await Bridge.saveAppRule(selectedPkg.value, '');
  } else {
    store.appRules[selectedPkg.value] = item.modeKey;
    await Bridge.saveAppRule(selectedPkg.value, item.modeKey);
  }
};
</script>

<template>
  <div class="app-rules">
    <van-nav-bar :title="t('app_management')" left-arrow @click-left="$router.back()" fixed placeholder />

    <!-- FAS 全局总开关 -->
    <div class="fas-card">
      <div class="fas-head">
        <van-icon name="fire-o" size="22" color="#E91E63" />
        <span class="fas-title">{{ t('fas_switch_title') }}</span>
        <van-switch
          v-model="fasEnabled"
          size="22px"
          active-color="#E91E63"
          :loading="fasLoading"
          @change="onFasToggle"
        />
      </div>
      <div class="fas-hint">
        {{ fasEnabled ? t('fas_switch_on_hint') : t('fas_switch_off_hint') }}
      </div>
    </div>

    <van-search v-model="searchText" :placeholder="t('search_apps')" />

    <van-list>
      <van-cell
        v-for="pkg in filteredApps"
        :key="pkg"
        :title="getLabel(pkg)"
        :label="pkg"
        center
        clickable
        @click="openMenu(pkg)"
      >
        <template #icon>
          <img
            :src="`ksu://icon/${pkg}`"
            style="width: 40px; height: 40px; margin-right: 12px; border-radius: 8px;"
            loading="lazy"
          />
        </template>
        <template #value>
          <van-tag v-if="store.appRules[pkg]" type="primary" size="medium">
            {{ modeLabel(store.appRules[pkg]) }}
          </van-tag>
          <span v-else class="no-rule">{{ t('not_configured') }}</span>
        </template>
      </van-cell>
    </van-list>

    <van-action-sheet
      v-model:show="showActionSheet"
      :actions="actions"
      :description="`${t('select_mode_for')} ${getLabel(selectedPkg)}`"
      :cancel-text="t('cancel')"
      @select="onSelectAction"
    />
  </div>
</template>

<style scoped>
.no-rule { font-size: 12px; color: #bbb; }

.fas-card {
  margin: 12px 12px 4px;
  padding: 14px 16px 12px;
  background: #fff;
  border-radius: 14px;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.05);
}
.fas-head {
  display: flex;
  align-items: center;
  gap: 8px;
}
.fas-title {
  flex: 1;
  font-size: 15px;
  font-weight: 700;
  color: #1f2329;
}
.fas-hint {
  margin-top: 8px;
  font-size: 12px;
  line-height: 1.6;
  color: #969799;
}
</style>
