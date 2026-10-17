<script setup lang="ts">
import { ref, onMounted, onUnmounted, reactive, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { exec } from '@/kernelsu';

const { t } = useI18n();

const MODDIR = '/data/adb/modules/aegis_power_matrix';
const CG_CONF = `${MODDIR}/charge_guard.conf`;
const CG_OFF = `${MODDIR}/charge_guard/data/off`;
const CG_STATUS = `${MODDIR}/charge_guard/data/status.kv`;
const CG_SKIP = `${MODDIR}/charge_guard/data/sched_skip`;

const NL = '~ZGNL~';

interface Kv { [k: string]: string }

async function execCmd(cmd: string): Promise<{ ok: boolean; stdout: string; stderr: string }> {
  try {
    const r: any = await exec(cmd);
    const bad = typeof r?.errno === 'number' && r.errno !== 0;
    return { ok: !bad, stdout: r?.stdout || '', stderr: r?.stderr || '' };
  } catch (e: any) {
    return { ok: false, stdout: '', stderr: String(e?.message || e) };
  }
}

async function readJoined(path: string): Promise<string> {
  const r = await execCmd(`sed 's/$/${NL}/' ${path} 2>/dev/null | tr -d '\\n'`);
  return (r.stdout || '').split(NL).join('\n');
}
function parseKv(text: string): Kv {
  const o: Kv = {};
  (text || '').split('\n').forEach((l) => {
    l = l.trim();
    if (!l || l[0] === '#') return;
    const i = l.indexOf('=');
    if (i > 0) o[l.slice(0, i).trim()] = l.slice(i + 1).trim();
  });
  return o;
}

// ── 本地 toast（不依赖管理器 toast 注入） ──
const toastMsg = ref('');
const toastErr = ref(false);
let toastTimer: any = null;
function toast(m: string, err = false) {
  toastMsg.value = (err ? '⚠️ ' : '✅ ') + m;
  toastErr.value = err;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (toastMsg.value = ''), 2500);
}

// ── 充电状态 ──
const cgOn = ref(false);
const statusText = ref('');
const schedText = ref('');
const confMissing = ref(false);
let loaded = false;
let dirty = false;

const form = reactive({
  power_stop: 80,
  power_start: 78,
  power_stop_time: 3,
  charge_full: 1,
  power_reset: 0,
  temperature_switch: 0,
  temperature_switch_stop: 45,
  temperature_switch_start: 40,
  sched_switch: 0,
  sched_time: '08:00',
  sched_margin: 15,
  sched_floor: 20,
  sched_window: 12,
});
let keepPowerSwitch: string[] = [];
const saving = ref(false);

// 用户一旦改动表单即标记 dirty，阻止 3 秒轮询回填覆盖未保存的编辑
// （现象：清空数字重新输入时，表单被磁盘旧值瞬间弹回）。
// flush:'sync' + populating 护栏保证 fillForm 的程序化赋值不会误标脏。
let populating = false;
watch(form, () => {
  if (!populating) dirty = true;
}, { deep: true, flush: 'sync' });

function fmtClock(sec: string): string {
  const d = new Date(Number(sec) * 1000);
  const p = (n: number) => String(n).padStart(2, '0');
  const now = new Date();
  const dd = Math.round(
    (new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime() -
      new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime()) / 864e5
  );
  return (dd === 1 ? '明天 ' : dd === 2 ? '后天 ' : '') + p(d.getHours()) + ':' + p(d.getMinutes());
}

function renderStatus(s: Kv) {
  if (!cgOn.value) {
    statusText.value = t('cg_off_hint') as string;
    schedText.value = '';
    return;
  }
  if (!s.updated_at) {
    statusText.value = t('cg_waiting') as string;
    schedText.value = '';
    return;
  }
  const lv = s.level || '--';
  const tmp = s.temp || '--';
  if (s.powered !== '1') {
    statusText.value = `${lv}% · ${tmp}°C · ${t('cg_not_charging')}`;
  } else if (s.stopped === '1') {
    // v1.3：stopped 标记存在但实际仍在充电 → 显示漏充告警
    if (s.leak === '1') {
      const node = (s.node || '').split('/').pop() || '--';
      statusText.value = t('cg_leak', { node }) as string;
    } else {
      const reasons: string[] = [];
      if (s.reason_battery === '1') reasons.push(t('cg_reason_battery') as string);
      if (s.reason_temp === '1') reasons.push(t('cg_reason_temp') as string);
      if (s.sched_held === '1') reasons.push(t('cg_reason_sched') as string);
      statusText.value = `${t('cg_stopped')}（${reasons.join('+') || t('cg_reason_battery')}）· ${lv}% · ${tmp}°C`;
    }
  } else {
    statusText.value = `${t('cg_charging')} · ${lv}% · ${tmp}°C`;
  }
  if (form.sched_switch !== 1) {
    schedText.value = '';
    return;
  }
  const st = parseInt(s.sched_state || '0', 10);
  const dl = fmtClock(s.sched_deadline || '0');
  const hl = (x: string) => x;
  if (s.sched_on === undefined) { schedText.value = '⚠️ 当前 charge_guardd 不支持预约充满'; return; }
  if (s.off === '1') { schedText.value = ''; return; }
  schedText.value =
    st === 1 ? `⏳ 已按住 ${hl(s.level || '--')}% · 预计 ${hl(fmtClock(s.sched_resume || '0'))} 开始充电 · 约 ${hl(fmtClock(s.sched_finish || '0'))} 充完（目标 ${hl(dl)}）`
    : st === 2 ? `⚡ 已放行，预计 ${hl(fmtClock(s.sched_finish || '0'))} 充完（目标 ${hl(dl)}）`
    : st === 3 ? '🔋 电量低于保底值，先正常充电'
    : st === 5 ? '✅ 已到目标电量'
    : st === 6 ? '⏭ 本次预约已跳过，明天照常生效'
    : `💤 待命：未插电，或距目标时刻（${dl}）较远`;
}

function fillForm(c: Kv) {
  populating = true;
  try {
    form.power_stop = parseInt(c.power_stop, 10) || 80;
    form.power_start = parseInt(c.power_start, 10) || 78;
    form.power_stop_time = parseInt(c.power_stop_time, 10) || 3;
    form.charge_full = c.charge_full === '1' ? 1 : 0;
    form.power_reset = c.power_reset === '1' ? 1 : 0;
    form.temperature_switch = c.temperature_switch === '1' ? 1 : 0;
    form.temperature_switch_stop = parseInt(c.temperature_switch_stop, 10) || 45;
    form.temperature_switch_start = parseInt(c.temperature_switch_start, 10) || 40;
    form.sched_switch = c.sched_switch === '1' ? 1 : 0;
    form.sched_time = /^\d{1,2}:\d{2}$/.test(c.sched_time || '') ? (c.sched_time as string).padStart(5, '0') : '08:00';
    const margin = parseInt(c.sched_margin, 10);
    const floor = parseInt(c.sched_floor, 10);
    form.sched_margin = Number.isNaN(margin) ? 15 : margin;
    form.sched_floor = Number.isNaN(floor) ? 20 : floor;
    form.sched_window = parseInt(c.sched_window, 10) || 12;
  } finally {
    populating = false;
  }
}

async function load() {
  try {
    const [ct, off, stt] = await Promise.all([
      readJoined(CG_CONF),
      execCmd(`[ -f ${CG_OFF} ] && echo off || echo on`),
      readJoined(CG_STATUS),
    ]);
    const c = parseKv(ct);
    const s = parseKv(stt);
    if (!c.power_stop) {
      confMissing.value = true;
      return;
    }
    confMissing.value = false;
    loaded = true;
    cgOn.value = (off.stdout || '').trim() !== 'off';
    if (!dirty) {
      fillForm(c);
      keepPowerSwitch = ct.split('\n').map((l) => l.trim()).filter((l) => /^power_switch=/.test(l));
    }
    renderStatus(s);
  } catch {
    confMissing.value = true;
  }
}

async function toggleCg(v: boolean) {
  await execCmd(v ? `rm -f ${CG_OFF}` : `touch ${CG_OFF}`);
  const chk = await execCmd(`[ -f ${CG_OFF} ] && echo off || echo on`);
  if (((chk.stdout || '').trim() === 'off') === v) {
    toast('toggle failed', true);
    return;
  }
  cgOn.value = v;
  toast(v ? '充电阈值控制已开启' : '充电阈值控制已关闭');
  load();
}

async function skipSched() {
  await execCmd(`touch ${CG_SKIP}`);
  toast(t('sched_skipped') as string);
  setTimeout(load, 12000);
}

async function save() {
  if (!loaded) {
    toast(t('cg_conf_missing') as string, true);
    return;
  }
  dirty = true;
  const errs: string[] = [];
  if (form.charge_full !== 1) {
    if (!(form.power_stop >= 1 && form.power_stop <= 100)) errs.push('停止电量需在 1-100');
    if (!(form.power_start >= 1 && form.power_start <= 100)) errs.push('恢复电量需在 1-100');
    if (form.power_stop <= form.power_start) errs.push('停止电量必须大于恢复电量');
    if (!(form.power_stop_time >= 1 && form.power_stop_time <= 120)) errs.push('停充延时需在 1-120 秒');
  }
  if (form.charge_full !== 0 && form.charge_full !== 1) errs.push('充满再停需为 0 或 1');
  if (form.power_reset !== 0 && form.power_reset !== 1) errs.push('自动拔插需为 0 或 1');
  if (form.temperature_switch) {
    if (!(form.temperature_switch_stop >= 25 && form.temperature_switch_stop <= 70 &&
          form.temperature_switch_start >= 25 && form.temperature_switch_start <= 70)) errs.push('温度需在 25-70℃');
    if (form.temperature_switch_stop <= form.temperature_switch_start) errs.push('停充温度必须大于恢复温度');
  }
  if (form.sched_switch) {
    if (!/^([01]\d|2[0-3]):[0-5]\d$/.test(form.sched_time)) errs.push('充满时刻格式 HH:MM');
    if (!(form.sched_margin >= 0 && form.sched_margin <= 180)) errs.push('安全余量 0-180');
    if (!(form.sched_floor >= 0 && form.sched_floor <= 90)) errs.push('保底电量 0-90');
    if (!(form.sched_window >= 1 && form.sched_window <= 16)) errs.push('生效窗口 1-16');
  }
  if (errs.length) {
    toast(t('cg_input_invalid', { msg: errs.join('；') }) as string, true);
    return;
  }
  const content = [
    '# charge_guard.conf — 充电阈值控制配置 (Aegis)',
    '# 修改后 3 秒内生效，无需重启。', '',
    '# 停止充电电量（1-100，需 > power_start）', `power_stop=${form.power_stop}`,
    '# 恢复充电电量', `power_start=${form.power_start}`,
    '# 停充延时（秒）', `power_stop_time=${form.power_stop_time}`,
    '# 充满再停 1/0', `charge_full=${form.charge_full}`,
    '# 自动阻断供电 1/0', `power_reset=${form.power_reset}`, '',
    '# 高温停充 1/0', `temperature_switch=${form.temperature_switch}`,
    `temperature_switch_stop=${form.temperature_switch_stop}`,
    `temperature_switch_start=${form.temperature_switch_start}`, '',
    '# 预约充满', `sched_switch=${form.sched_switch}`,
    `sched_time=${form.sched_time}`,
    `sched_margin=${form.sched_margin}`,
    `sched_floor=${form.sched_floor}`,
    `sched_window=${form.sched_window}`, '',
    '# 自定义供电开关：power_switch=[路径 start=开值 stop=关值]',
    ...keepPowerSwitch, '',
  ].join('\n');

  saving.value = true;
  try {
    await execCmd(`cat > ${CG_CONF} << 'ZGEOF'\n${content}\nZGEOF`);
    const c = parseKv(await readJoined(CG_CONF));
    const ok =
      String(c.power_stop) === String(form.power_stop) &&
      String(c.power_start) === String(form.power_start) &&
      String(c.temperature_switch_stop) === String(form.temperature_switch_stop) &&
      String(c.sched_switch) === String(form.sched_switch) &&
      String(c.sched_window) === String(form.sched_window);
    if (!ok) {
      toast(t('cg_verify_mismatch') as string, true);
      return;
    }
    dirty = false;
    toast(t('settings_saved') as string);
    load();
  } finally {
    saving.value = false;
  }
}

let timer: any = null;
onMounted(async () => {
  await load();
  timer = setInterval(load, 3000);
});
onUnmounted(() => clearInterval(timer));
</script>

<template>
  <div class="charge-page">
    <van-nav-bar
      :title="t('menu_charge')"
      left-arrow
      @click-left="$router.back()"
      fixed
      placeholder
    />

    <!-- 充电阈值 -->
    <van-cell-group inset class="cg-group">
      <van-cell center>
        <template #title>
          <span class="cg-title">{{ t('cg_title') }}</span>
        </template>
        <template #right-icon>
          <van-switch :model-value="cgOn" @update:model-value="toggleCg" />
        </template>
      </van-cell>
      <van-cell v-if="cgOn">
        <template #title>
          <div class="cg-status">{{ statusText }}</div>
          <div v-if="schedText" class="cg-sched">{{ schedText }}</div>
          <div v-if="confMissing" class="cg-err">{{ t('cg_conf_missing') }}</div>
        </template>
      </van-cell>
    </van-cell-group>

    <template v-if="cgOn && !confMissing">
      <van-cell-group inset class="cg-group">
        <van-field v-model.number="form.power_stop" type="number" :label="t('power_stop')" input-align="right" :disabled="form.charge_full === 1" />
        <van-field v-model.number="form.power_start" type="number" :label="t('power_start')" input-align="right" :disabled="form.charge_full === 1" />
        <van-field v-model.number="form.power_stop_time" type="number" :label="t('power_stop_time')" input-align="right" :disabled="form.charge_full === 1" />
        <van-cell center :title="t('charge_full')">
          <template #right-icon>
            <van-switch v-model="form.charge_full" :active-value="1" :inactive-value="0" />
          </template>
        </van-cell>
        <van-cell center :title="t('power_reset')">
          <template #right-icon>
            <van-switch v-model="form.power_reset" :active-value="1" :inactive-value="0" />
          </template>
        </van-cell>
      </van-cell-group>

      <van-cell-group inset class="cg-group">
        <van-cell center :title="t('temp_guard')">
          <template #right-icon>
            <van-switch v-model="form.temperature_switch" :active-value="1" :inactive-value="0" />
          </template>
        </van-cell>
        <template v-if="form.temperature_switch === 1">
          <van-field v-model.number="form.temperature_switch_stop" type="number" :label="t('temp_stop')" input-align="right" />
          <van-field v-model.number="form.temperature_switch_start" type="number" :label="t('temp_start')" input-align="right" />
        </template>
      </van-cell-group>

      <van-cell-group inset class="cg-group">
        <van-cell center :title="t('sched_guard')">
          <template #right-icon>
            <van-switch v-model="form.sched_switch" :active-value="1" :inactive-value="0" />
          </template>
        </van-cell>
        <template v-if="form.sched_switch === 1">
          <van-field v-model="form.sched_time" type="time" :label="t('sched_time')" input-align="right" />
          <van-field v-model.number="form.sched_margin" type="number" :label="t('sched_margin')" input-align="right" />
          <van-field v-model.number="form.sched_floor" type="number" :label="t('sched_floor')" input-align="right" />
          <van-field v-model.number="form.sched_window" type="number" :label="t('sched_window')" input-align="right" />
          <van-cell>
            <template #title>
              <van-button size="small" plain type="warning" @click="skipSched">{{ t('sched_skip') }}</van-button>
            </template>
          </van-cell>
        </template>
      </van-cell-group>

      <div class="save-wrap">
        <van-button type="primary" block round :loading="saving" @click="save">{{ t('save_settings') }}</van-button>
      </div>
    </template>

    <transition name="fade">
      <div v-if="toastMsg" class="cg-toast" :class="{ err: toastErr }">{{ toastMsg }}</div>
    </transition>
  </div>
</template>

<style scoped>
.charge-page {
  padding-bottom: 40px;
  background: #f7f8fa;
  min-height: 100vh;
}
.cg-group {
  margin-top: 12px;
  border-radius: 12px;
  overflow: hidden;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.04);
}
.cg-title {
  font-weight: 600;
  font-size: 15px;
}
.cg-status {
  font-size: 14px;
  color: #323233;
  line-height: 1.6;
}
.cg-sched {
  font-size: 12px;
  color: #2196f3;
  line-height: 1.6;
  margin-top: 2px;
}
.cg-err {
  font-size: 12px;
  color: #ee0a24;
  margin-top: 4px;
}
.save-wrap {
  margin: 20px 24px 0;
}
.cg-toast {
  position: fixed;
  left: 50%;
  bottom: 60px;
  transform: translateX(-50%);
  background: rgba(34, 34, 34, 0.88);
  color: #fff;
  padding: 10px 18px;
  border-radius: 22px;
  font-size: 13px;
  max-width: 86vw;
  z-index: 999;
}
.cg-toast.err {
  background: rgba(238, 10, 36, 0.92);
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
