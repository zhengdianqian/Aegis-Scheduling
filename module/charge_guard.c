/*
 * charge_guard.c — 充电阈值控制（止观 扩展功能）单进程守护
 * 替代原 charge_guard 目录下的 shell 脚本（switch.sh + loop.sh）：
 * 单进程常驻、sleep() 挂起，
 * 直接 open/read/write 操作 sysfs，不再每轮 fork dumpsys/cat/sed/awk。
 *
 * 用法：
 *   charge_guard <moddir>            前台运行主循环（service.sh 用 & 拉起）
 *   charge_guard <moddir> restore    卸载前把所有已知停充节点恢复为“开始充电”值
 *   charge_guard <moddir> rescan     重新扫描 list_switch（等价于原 list_switch.sh 的触发点）
 *   charge_guard -v | --version      仅打印版本号后退出（供 customize.sh 轻量验证）
 *
 * 【预约充满】(sched_*)：在设定时刻前把电量充到目标值（充满即停开启时=涓流结束，
 *   否则=power_stop）。做法是“先保持、再放行”：插电后用现有的停充节点把电量按住，
 *   按自学习的充电曲线算出“最晚开始充电时刻”，到点放行、全速充，刚好在设定时刻前
 *   完成。只用已验证可用的停充节点，不依赖任何限流节点。见文件中 SCHED 一节。
 *
 * 与 shell 版的关键差异（有意为之）：
 *   - 停充/恢复的“是否已停充”“因电量还是因温度”“涓流计数”等状态，shell 版靠
 *     touch/rm 标记文件在多次独立进程间传递；这里是常驻单进程，直接用全局变量
 *     保存状态，不再需要那些标记文件（只保留 WebUI 需要读的 off 开关和 status.kv）。
 *   - list_switch 的节点扫描（find /sys/... 数十条）保留在 list_switch.sh 里不变：
 *     这是开机后只需要跑一次的一次性发现过程，用 shell+find 写来最清楚，
 *     没必要为了“全部是 C”而增加维护成本；C 侧只负责按扫描结果里的路径做
 *     高频写入，这才是真正需要省开销的部分。
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>

#define MODULE_BASE "/data/adb/modules/thread_affinity_opt_8gen2"
#define MAX_SWITCHES 512
#define MAX_LINE 512

typedef struct {
    char path[256];
    char start[40];
    char stop[40];
} switch_entry_t;

typedef struct {
    char power_switch_path[256];
    char power_switch_start[40];
    char power_switch_stop[40];
} custom_switch_t;

/* ===================== 路径 ===================== */
static char g_moddir[256];
static char g_conf[300];
static char g_bindir[300];
static char g_datadir[300];
static char g_list_switch[320];
static char g_device_profile[320];
static char g_off_flag[320];
static char g_status_kv[320];
static char g_curve_file[320];
static char g_sched_skip[320];

static void build_paths(const char *moddir)
{
    snprintf(g_moddir, sizeof(g_moddir), "%s", moddir);
    snprintf(g_conf, sizeof(g_conf), "%s/charge_guard.conf", moddir);
    snprintf(g_bindir, sizeof(g_bindir), "%s/charge_guard", moddir);
    snprintf(g_datadir, sizeof(g_datadir), "%s/charge_guard/data", moddir);
    snprintf(g_list_switch, sizeof(g_list_switch), "%s/list_switch", g_datadir);
    snprintf(g_device_profile, sizeof(g_device_profile), "%s/device.profile", g_datadir);
    snprintf(g_off_flag, sizeof(g_off_flag), "%s/off", g_datadir);
    snprintf(g_status_kv, sizeof(g_status_kv), "%s/status.kv", g_datadir);
    snprintf(g_curve_file, sizeof(g_curve_file), "%s/charge_curve.kv", g_datadir);
    snprintf(g_sched_skip, sizeof(g_sched_skip), "%s/sched_skip", g_datadir);
}

/* ===================== 基础 IO ===================== */
static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* 读取整段文本（去掉行尾 \n），失败返回 -1 */
static int read_line_trim(const char *path, char *buf, size_t sz)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)sz, f)) { fclose(f); return -1; }
    fclose(f);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    return 0;
}

/* 直接写 sysfs 节点，成功返回 0 */
static int write_node(const char *path, const char *val)
{
    if (!file_exists(path)) return -1;
    chmod(path, 0666);
    int fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) return -1;
    ssize_t len = (ssize_t)strlen(val);
    ssize_t n = write(fd, val, (size_t)len);
    close(fd);
    return (n == len) ? 0 : -1;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/* ===================== 配置解析 ===================== */
typedef struct {
    int power_stop;
    int power_start;
    int power_stop_time;
    int charge_full;
    int power_reset;
    int temperature_switch;
    int temperature_switch_stop;
    int temperature_switch_start;
    int sched_switch;   /* 预约充满总开关 0/1 */
    int sched_hour;     /* 目标时刻 时 0-23 */
    int sched_min;      /* 目标时刻 分 0-59 */
    int sched_margin;   /* 安全余量（分钟）：提前这么久充完 */
    int sched_floor;    /* 保底电量（%）：低于它不按住，先充 */
    int sched_window;   /* 生效窗口（小时）：距目标时刻超过它则不预约，照常充 */
    custom_switch_t custom[16];
    int custom_n;
} config_t;

static int clamp_int(int v, int lo, int hi, int dflt)
{
    if (v < lo || v > hi) return dflt;
    return v;
}

/* 解析形如 power_switch=[/path/to/node start=0 stop=1] 的自定义节点行 */
static void parse_power_switch_line(const char *val, config_t *cfg)
{
    if (cfg->custom_n >= 16) return;
    const char *lb = strchr(val, '[');
    const char *rb = strchr(val, ']');
    if (!lb || !rb || rb <= lb) return;
    char inner[256];
    size_t n = (size_t)(rb - lb - 1);
    if (n >= sizeof(inner)) n = sizeof(inner) - 1;
    memcpy(inner, lb + 1, n);
    inner[n] = '\0';

    char path[256] = {0}, start[40] = {0}, stop[40] = {0};
    char *tok = strtok(inner, " \t");
    if (tok) snprintf(path, sizeof(path), "%s", tok);
    while ((tok = strtok(NULL, " \t")) != NULL) {
        if (!strncmp(tok, "start=", 6)) snprintf(start, sizeof(start), "%s", tok + 6);
        else if (!strncmp(tok, "stop=", 5)) snprintf(stop, sizeof(stop), "%s", tok + 5);
    }
    if (path[0] && start[0] && stop[0]) {
        custom_switch_t *c = &cfg->custom[cfg->custom_n++];
        snprintf(c->power_switch_path, sizeof(c->power_switch_path), "%s", path);
        snprintf(c->power_switch_start, sizeof(c->power_switch_start), "%s", start);
        snprintf(c->power_switch_stop, sizeof(c->power_switch_stop), "%s", stop);
    }
}

static void load_config(config_t *cfg)
{
    /* 缺省值：文件读不到/字段缺失时的兜底，与 charge_guard.conf 出厂默认一致 */
    cfg->power_stop = 90;
    cfg->power_start = 80;
    cfg->power_stop_time = 3;
    cfg->charge_full = 0;
    cfg->power_reset = 0;
    cfg->temperature_switch = 1;
    cfg->temperature_switch_stop = 45;
    cfg->temperature_switch_start = 40;
    cfg->sched_switch = 0;
    cfg->sched_hour = 8;
    cfg->sched_min = 0;
    cfg->sched_margin = 15;
    cfg->sched_floor = 20;
    cfg->sched_window = 12;
    cfg->custom_n = 0;

    FILE *f = fopen(g_conf, "r");
    if (!f) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (p[0] == '\0' || p[0] == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);
        if (!strcmp(key, "power_stop")) cfg->power_stop = atoi(val);
        else if (!strcmp(key, "power_start")) cfg->power_start = atoi(val);
        else if (!strcmp(key, "power_stop_time")) cfg->power_stop_time = atoi(val);
        else if (!strcmp(key, "charge_full")) cfg->charge_full = atoi(val);
        else if (!strcmp(key, "power_reset")) cfg->power_reset = atoi(val);
        else if (!strcmp(key, "temperature_switch")) cfg->temperature_switch = atoi(val);
        else if (!strcmp(key, "temperature_switch_stop")) cfg->temperature_switch_stop = atoi(val);
        else if (!strcmp(key, "temperature_switch_start")) cfg->temperature_switch_start = atoi(val);
        else if (!strcmp(key, "sched_switch")) cfg->sched_switch = atoi(val);
        else if (!strcmp(key, "sched_time")) {
            int h, m;
            if (sscanf(val, "%d:%d", &h, &m) == 2 && h >= 0 && h <= 23 && m >= 0 && m <= 59) {
                cfg->sched_hour = h; cfg->sched_min = m;
            }
        }
        else if (!strcmp(key, "sched_margin")) cfg->sched_margin = atoi(val);
        else if (!strcmp(key, "sched_floor")) cfg->sched_floor = atoi(val);
        else if (!strcmp(key, "sched_window")) cfg->sched_window = atoi(val);
        else if (!strcmp(key, "power_switch")) parse_power_switch_line(val, cfg);
    }
    fclose(f);

    /* 钳位与出厂 switch.sh 完全一致 */
    cfg->charge_full = clamp_int(cfg->charge_full, 0, 1, 0);
    cfg->power_reset = clamp_int(cfg->power_reset, 0, 1, 0);
    if (cfg->power_stop < 1 || cfg->power_stop > 110) cfg->power_stop = 100;
    cfg->power_start = clamp_int(cfg->power_start, 1, 100, 95);
    if (cfg->power_stop <= 100 && cfg->power_stop <= cfg->power_start) {
        cfg->power_start = (cfg->power_stop > 5) ? cfg->power_stop - 5 : 1;
    }
    cfg->power_stop_time = clamp_int(cfg->power_stop_time, 1, 120, 3);
    cfg->temperature_switch = clamp_int(cfg->temperature_switch, 0, 1, 1);
    cfg->temperature_switch_stop = clamp_int(cfg->temperature_switch_stop, 25, 70, 60);
    cfg->temperature_switch_start = clamp_int(cfg->temperature_switch_start, 25, 70, 50);
    if (cfg->temperature_switch_stop <= cfg->temperature_switch_start) {
        cfg->temperature_switch_start = (cfg->temperature_switch_stop > 30)
            ? cfg->temperature_switch_stop - 5 : 25;
    }
    cfg->sched_switch = clamp_int(cfg->sched_switch, 0, 1, 0);
    cfg->sched_margin = clamp_int(cfg->sched_margin, 0, 180, 15);
    cfg->sched_floor = clamp_int(cfg->sched_floor, 0, 90, 20);
    cfg->sched_window = clamp_int(cfg->sched_window, 1, 16, 12);
}

/* ===================== 设备开关列表 ===================== */

/* 各机型常见停充节点兜底，与 list_switch.sh 扫描互补（一次编译进二进制，无需再读文件） */
static const switch_entry_t g_fallback[] = {
    {"/sys/class/power_supply/battery/charge_charger_state", "1", "0"},
    {"/sys/class/power_supply/battery/connect_disable", "0", "1"},
    {"/sys/class/power_supply/battery/batt_slate_mode", "0", "1"},
    {"/sys/class/power_supply/battery/store_mode", "0", "1"},
    {"/sys/class/power_supply/battery/input_suspend", "0", "1"},
    {"/sys/class/power_supply/battery/battery_input_suspend", "0", "1"},
    {"/sys/class/power_supply/battery/night_charging", "0", "1"},
    {"/sys/class/power_supply/battery/charging_enabled", "1", "0"},
    {"/sys/class/power_supply/battery/battery_charging_enabled", "1", "0"},
    {"/sys/class/power_supply/battery/batt_charging_enabled", "1", "0"},
    {"/sys/class/power_supply/battery/charge_disable", "0", "1"},
    {"/sys/class/power_supply/battery/disable_charging", "0", "1"},
    {"/sys/class/power_supply/battery/stop_charging", "0", "1"},
    {"/sys/class/power_supply/battery/charge_enabled", "1", "0"},
    {"/sys/class/power_supply/battery/op_disable_charge", "0", "1"},
    {"/sys/class/power_supply/battery/device/Charging_Enable", "1", "0"},
    {"/sys/class/power_supply/battery/ChargerEnable", "1", "0"},
    {"/sys/class/power_supply/battery/force_disable_charging", "0", "1"},
    {"/sys/class/power_supply/battery/charge_control_enabled", "1", "0"},
    {"/sys/class/power_supply/battery/mi_charge_enable", "1", "0"},
    {"/sys/class/power_supply/battery_ext/smart_charging_interruption", "0", "1"},
    {"/sys/class/power_supply/main/adapter_cc_mode", "0", "1"},
    {"/sys/class/power_supply/main/cool_mode", "0", "1"},
    {"/sys/class/power_supply/charger/charge_disable", "0", "1"},
    {"/sys/class/power_supply/bms/charge_disable", "0", "1"},
    {"/sys/class/power_supply/bms/charging_enabled", "1", "0"},
    {"/sys/class/power_supply/bms/charge_enabled", "1", "0"},
    {"/sys/class/power_supply/mi_chg/charge_disable", "0", "1"},
    {"/sys/class/power_supply/mi_chg/charging_enabled", "1", "0"},
    {"/sys/class/qcom-battery/charging_enabled", "1", "0"},
    {"/sys/class/qcom-battery/charge_disable", "0", "1"},
    {"/sys/class/qcom-battery/input_suspend", "0", "1"},
    {"/sys/class/qcom-battery/battery_charging_enabled", "1", "0"},
    {"/sys/class/qcom-battery/charging_suspend_battery", "0", "1"},
    {"/sys/class/qcom-battery/night_charging", "0", "1"},
    {"/sys/class/qcom-battery/cool_mode", "0", "1"},
    {"/sys/class/qcom-battery/batt_protect_en", "0", "1"},
    {"/sys/class/asuslib/charger_limit_en", "0", "1"},
    {"/sys/class/asuslib/charging_suspend_en", "0", "1"},
    {"/sys/class/hw_power/charger/charge_data/enable_charger", "1", "0"},
    {"/sys/devices/platform/huawei_charger/enable_charger", "1", "0"},
    {"/sys/devices/platform/lge-unified-nodes/charging_enable", "1", "0"},
    {"/sys/devices/platform/lge-unified-nodes/charging_completed", "0", "1"},
    {"/sys/devices/platform/mt-battery/disable_charger", "0", "1"},
    {"/sys/devices/platform/battery/ChargerEnable", "1", "0"},
    {"/sys/devices/platform/soc/soc:google,charger/charge_disable", "0", "1"},
    {"/sys/devices/platform/charger/bypass_charger", "0", "1"},
    {"/sys/devices/virtual/oplus_chg/battery/mmi_charging_enable", "1", "0"},
    {"/sys/kernel/debug/google_charger/chg_suspend", "0", "1"},
    {"/sys/kernel/debug/google_charger/input_suspend", "0", "1"},
    {"/sys/kernel/debug/google_charger/chg_mode", "1", "0"},
    {"/sys/kernel/nubia_charge/charger_bypass", "off", "on"},
    {"/sys/module/qpnp_adaptive_charge/parameters/blocking", "0", "1"},
    {"/sys/class/power_supply/usb/input_suspend", "0", "1"},
    {"/sys/class/power_supply/qc_usb/input_suspend", "0", "1"},
    {"/sys/class/power_supply/dc/input_suspend", "0", "1"},
    {"/sys/class/power_supply/wireless/input_suspend", "0", "1"},
    {"/sys/class/power_supply/pc_port/input_suspend", "0", "1"},
    {"/sys/class/power_supply/battery/charge_type", "Fast", "None"},
};
#define FALLBACK_N (int)(sizeof(g_fallback) / sizeof(g_fallback[0]))

static switch_entry_t g_switches[MAX_SWITCHES];
static int g_switch_n = 0;

/* preferred_switch：上次实测成功的节点，来自 device.profile，优先尝试，减少逐条探测 */
static switch_entry_t g_preferred;
static int g_has_preferred = 0;
static switch_entry_t g_mca;
static int g_has_mca = 0;

static void load_device_profile(void)
{
    g_has_preferred = 0;
    g_has_mca = 0;
    FILE *f = fopen(g_device_profile, "r");
    if (!f) return;
    char line[MAX_LINE];
    char mca_path[256] = {0}, mca_flag[8] = {0};
    char pref_path[256] = {0}, pref_start[40] = {0}, pref_stop[40] = {0};
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (p[0] == '\0' || p[0] == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);
        if (!strcmp(key, "mca")) snprintf(mca_flag, sizeof(mca_flag), "%s", val);
        else if (!strcmp(key, "mca_path")) snprintf(mca_path, sizeof(mca_path), "%s", val);
        else if (!strcmp(key, "preferred_switch")) snprintf(pref_path, sizeof(pref_path), "%s", val);
        else if (!strcmp(key, "preferred_start")) snprintf(pref_start, sizeof(pref_start), "%s", val);
        else if (!strcmp(key, "preferred_stop")) snprintf(pref_stop, sizeof(pref_stop), "%s", val);
    }
    fclose(f);

    if (!strcmp(mca_flag, "1") && mca_path[0] && file_exists(mca_path)) {
        snprintf(g_mca.path, sizeof(g_mca.path), "%s", mca_path);
        snprintf(g_mca.start, sizeof(g_mca.start), "0");
        snprintf(g_mca.stop, sizeof(g_mca.stop), "1");
        g_has_mca = 1;
    }
    if (pref_path[0] && pref_start[0] && pref_stop[0] && file_exists(pref_path)) {
        snprintf(g_preferred.path, sizeof(g_preferred.path), "%s", pref_path);
        snprintf(g_preferred.start, sizeof(g_preferred.start), "%s", pref_start);
        snprintf(g_preferred.stop, sizeof(g_preferred.stop), "%s", pref_stop);
        g_has_preferred = 1;
    }
}

/* 把实测成功的节点写回 device.profile，下次重启直接命中，不用重新逐条探测 */
static void save_preferred_switch(const switch_entry_t *e)
{
    char tmp[400];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_device_profile);
    FILE *in = fopen(g_device_profile, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return; }
    int wrote_switch = 0, wrote_start = 0, wrote_stop = 0, wrote_at = 0;
    char line[MAX_LINE];
    time_t now = time(NULL);
    struct tm tmv; localtime_r(&now, &tmv);
    char tsbuf[32];
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d_%H:%M:%S", &tmv);

    if (in) {
        while (fgets(line, sizeof(line), in)) {
            if (!strncmp(line, "preferred_switch=", 17)) {
                fprintf(out, "preferred_switch=%s\n", e->path); wrote_switch = 1;
            } else if (!strncmp(line, "preferred_start=", 16)) {
                fprintf(out, "preferred_start=%s\n", e->start); wrote_start = 1;
            } else if (!strncmp(line, "preferred_stop=", 15)) {
                fprintf(out, "preferred_stop=%s\n", e->stop); wrote_stop = 1;
            } else if (!strncmp(line, "preferred_tested_at=", 20)) {
                fprintf(out, "preferred_tested_at=%s\n", tsbuf); wrote_at = 1;
            } else {
                fputs(line, out);
            }
        }
        fclose(in);
    }
    if (!wrote_switch) fprintf(out, "preferred_switch=%s\n", e->path);
    if (!wrote_start)  fprintf(out, "preferred_start=%s\n", e->start);
    if (!wrote_stop)   fprintf(out, "preferred_stop=%s\n", e->stop);
    if (!wrote_at)     fprintf(out, "preferred_tested_at=%s\n", tsbuf);
    fclose(out);
    rename(tmp, g_device_profile);

    g_preferred = *e;
    g_has_preferred = 1;
}

/* 加载 list_switch.sh 扫描出的节点文件，格式：path,start=X,stop=Y */
static void load_list_switch(void)
{
    g_switch_n = 0;
    FILE *f = fopen(g_list_switch, "r");
    if (!f) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f) && g_switch_n < MAX_SWITCHES) {
        char *p = trim(line);
        if (p[0] == '\0' || p[0] == '#') continue;
        char *c1 = strchr(p, ',');
        if (!c1) continue;
        char *c2 = strchr(c1 + 1, ',');
        if (!c2) continue;
        *c1 = '\0'; *c2 = '\0';
        char *path = p;
        char *start_kv = c1 + 1; /* start=X */
        char *stop_kv = c2 + 1;  /* stop=Y */
        if (strncmp(start_kv, "start=", 6) || strncmp(stop_kv, "stop=", 5)) continue;
        switch_entry_t *e = &g_switches[g_switch_n++];
        snprintf(e->path, sizeof(e->path), "%s", path);
        snprintf(e->start, sizeof(e->start), "%s", start_kv + 6);
        snprintf(e->stop, sizeof(e->stop), "%s", stop_kv + 5);
    }
    fclose(f);
}

/* 惰性初始化：只有真的要写节点时才调用，日常轮询不需要 */
static int g_switches_ready = 0;
static config_t *g_cfg_for_switches = NULL;
static void ensure_switches_loaded(config_t *cfg)
{
    if (g_switches_ready) return;
    load_device_profile();
    load_list_switch();
    g_cfg_for_switches = cfg;
    g_switches_ready = 1;
}

/* 依次尝试：preferred -> MCA -> list_switch 扫描结果 -> 自定义 power_switch -> 静态兜底表，
 * 命中即停，成功后把该节点记为 preferred，加速以后的判定 */
static int try_write(const char *value_field_is_start)
{
    int want_start = !strcmp(value_field_is_start, "start");

    if (g_has_preferred) {
        const char *v = want_start ? g_preferred.start : g_preferred.stop;
        if (write_node(g_preferred.path, v) == 0) return 0;
    }
    if (g_has_mca) {
        const char *v = want_start ? g_mca.start : g_mca.stop;
        if (write_node(g_mca.path, v) == 0) { save_preferred_switch(&g_mca); return 0; }
    }
    for (int i = 0; i < g_switch_n; i++) {
        const char *v = want_start ? g_switches[i].start : g_switches[i].stop;
        if (write_node(g_switches[i].path, v) == 0) { save_preferred_switch(&g_switches[i]); return 0; }
    }
    if (g_cfg_for_switches) {
        for (int i = 0; i < g_cfg_for_switches->custom_n; i++) {
            custom_switch_t *c = &g_cfg_for_switches->custom[i];
            const char *v = want_start ? c->power_switch_start : c->power_switch_stop;
            if (write_node(c->power_switch_path, v) == 0) {
                switch_entry_t e;
                snprintf(e.path, sizeof(e.path), "%s", c->power_switch_path);
                snprintf(e.start, sizeof(e.start), "%s", c->power_switch_start);
                snprintf(e.stop, sizeof(e.stop), "%s", c->power_switch_stop);
                save_preferred_switch(&e);
                return 0;
            }
        }
    }
    for (int i = 0; i < FALLBACK_N; i++) {
        const char *v = want_start ? g_fallback[i].start : g_fallback[i].stop;
        if (write_node(g_fallback[i].path, v) == 0) { save_preferred_switch(&g_fallback[i]); return 0; }
    }
    return -1;
}

/* ===================== 电池读数 ===================== */
typedef struct {
    int level;      /* -1 = 未知 */
    int temp_c;     /* -1000 = 未知 */
    int powered;    /* 0/1 */
    int status_code; /* 2=Charging 5=Full 3=Discharging 4=NotCharging 0=未知 */
} battery_t;

static int read_sysfs_int(const char *path)
{
    char buf[32];
    if (read_line_trim(path, buf, sizeof(buf)) != 0) return INT_MIN;
    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf) return INT_MIN;
    return (int)v;
}

/* /sys 温度单位可能是 milli(°C*1000，thermal_zone/部分 IIO)、0.1°C(power_supply) 或 °C。
 * |raw|>=1000 → milli；101~999 → 0.1°C；其余当 °C。 */
static int normalize_temp(int raw)
{
    if (raw == INT_MIN) return INT_MIN;
    int a = (raw < 0) ? -raw : raw;
    if (a >= 1000) return raw / 1000;
    if (a > 100)   return raw / 10;
    return raw;
}

/* 物理合理性：电池工作温度 -20~85°C；恒 0 一律视为无效桩节点
 *（实测 dizi/HyperOS 的 battery/temp、thermal_zone battery、dumpsys 全部恒为 0）。 */
static int temp_plausible(int t)
{
    return t != INT_MIN && t != 0 && t >= -20 && t <= 85;
}

static int read_temp_candidate(const char *path)
{
    int v = read_sysfs_int(path);
    if (v == INT_MIN) return INT_MIN;
    int t = normalize_temp(v);
    return temp_plausible(t) ? t : INT_MIN;
}

/* IIO 兜底：8Gen3(dizi) 等新 HyperOS 上标准电池温度节点全是 0 桩，
 * 真实热敏电阻只挂在 PMIC VADC（/sys/bus/iio/devices/iio:deviceN/in_temp_bat*therm*_input）。
 * 优先无后缀主通道（in_temp_bat_therm_input），30k、400k 上拉通道作备选；
 * 必须含 "therm"，排除 in_temp_bat_id_input（电池 ID 电阻，读数近百万）。 */
static int read_iio_bat_temp(void)
{
    int alt = INT_MIN;
    DIR *root = opendir("/sys/bus/iio/devices");
    if (!root) return INT_MIN;
    struct dirent *dev;
    while ((dev = readdir(root))) {
        if (strncmp(dev->d_name, "iio:device", 10) != 0) continue;
        char dirp[160];
        snprintf(dirp, sizeof(dirp), "/sys/bus/iio/devices/%s", dev->d_name);
        DIR *dd = opendir(dirp);
        if (!dd) continue;
        struct dirent *f;
        while ((f = readdir(dd))) {
            const char *n = f->d_name;
            if (strncmp(n, "in_temp_bat", 11) != 0 || !strstr(n, "therm")) continue;
            char fp[256];
            snprintf(fp, sizeof(fp), "%s/%s", dirp, n);
            int t = read_temp_candidate(fp);
            if (t == INT_MIN) continue;
            if (!strcmp(n, "in_temp_bat_therm_input")) { closedir(dd); closedir(root); return t; }
            if (alt == INT_MIN) alt = t;
        }
        closedir(dd);
    }
    closedir(root);
    return alt;
}

/* thermal_zone 兜底：type 为 battery 的 zone（单位 milli°C）。
 * dizi 上该节点同样是 0 桩，会被 temp_plausible 拒绝。 */
static int read_thermal_zone_bat_temp(void)
{
    int best = INT_MIN;
    DIR *root = opendir("/sys/class/thermal");
    if (!root) return INT_MIN;
    struct dirent *de;
    while ((de = readdir(root))) {
        if (strncmp(de->d_name, "thermal_zone", 12) != 0) continue;
        char tp[128], buf[16] = {0};
        snprintf(tp, sizeof(tp), "/sys/class/thermal/%s/type", de->d_name);
        if (read_line_trim(tp, buf, sizeof(buf)) != 0) continue;
        if (strcmp(buf, "battery") != 0) continue;
        char fp[160];
        snprintf(fp, sizeof(fp), "/sys/class/thermal/%s/temp", de->d_name);
        int t = read_temp_candidate(fp);
        if (t != INT_MIN) { best = t; break; }
    }
    closedir(root);
    return best;
}

static void read_battery_sysfs(battery_t *b)
{
    char status[32] = {0};
    b->status_code = 0;
    b->powered = 0;
    if (read_line_trim("/sys/class/power_supply/battery/status", status, sizeof(status)) == 0) {
        if (!strcmp(status, "Charging")) { b->status_code = 2; b->powered = 1; }
        else if (!strcmp(status, "Full")) { b->status_code = 5; b->powered = 1; }
        else if (!strcmp(status, "Discharging")) { b->status_code = 3; }
        else if (!strcmp(status, "Not charging")) { b->status_code = 4; }
    }
    int lvl = read_sysfs_int("/sys/class/power_supply/battery/capacity");
    b->level = (lvl == INT_MIN) ? -1 : lvl;
    /* 标准 power_supply 节点 → PMIC VADC(IIO) → thermal_zone，取首个物理合理值 */
    static const char *temp_nodes[] = {
        "/sys/class/power_supply/battery/temp",
        "/sys/class/power_supply/bms/temp",
        "/sys/class/power_supply/battery/batt_temp",
        "/sys/class/power_supply/mi_batt/temp",
        NULL
    };
    int t = INT_MIN;
    for (int i = 0; temp_nodes[i] && t == INT_MIN; i++)
        t = read_temp_candidate(temp_nodes[i]);
    if (t == INT_MIN) t = read_iio_bat_temp();
    if (t == INT_MIN) t = read_thermal_zone_bat_temp();
    b->temp_c = temp_plausible(t) ? t : -1000;

    if (!b->powered) {
        static const char *usb_nodes[] = {
            "/sys/class/power_supply/usb/online",
            "/sys/class/power_supply/qc_usb/online",
            NULL
        };
        for (int i = 0; usb_nodes[i]; i++) {
            int v = read_sysfs_int(usb_nodes[i]);
            if (v == 1) { b->powered = 1; if (!b->status_code) b->status_code = 2; break; }
        }
    }
}

/* dumpsys battery 兜底：仅在 sysfs 三项读不全时才用，避免每轮都 fork+Binder IPC */
static void read_battery_dumpsys_fallback(battery_t *b)
{
    if (b->level >= 0 && b->status_code != 0 && b->temp_c != -1000) return;
    FILE *p = popen("dumpsys battery 2>/dev/null", "r");
    if (!p) return;
    char line[256];
    while (fgets(line, sizeof(line), p)) {
        char *l = trim(line);
        if (b->level < 0 && !strncmp(l, "level:", 6)) {
            b->level = atoi(l + 6);
        } else if (!strncmp(l, "status:", 7)) {
            int code = atoi(l + 7);
            if (b->status_code == 0) b->status_code = code;
            if (code == 2 || code == 5) b->powered = 1;
        } else if (b->temp_c == -1000 && !strncmp(l, "temperature:", 12)) {
            int dt = normalize_temp(atoi(l + 12));
            if (temp_plausible(dt)) b->temp_c = dt;
        }
    }
    pclose(p);
}

/* ===================== SCHED：预约充满 =====================
 * 目标：在 sched_time 之前把电量充到目标值。
 *   目标值 = charge_full 开启 ? 100（含涓流收尾） : power_stop（≤100，否则按 100）
 *
 * 思路（“先按住、再放行”）：
 *   1. 插电且距目标时刻在 sched_window 小时以内 → 用现有停充节点把电量按住（HOLD）；
 *   2. 每轮用“自学习充电曲线”估算 电量→目标 还需要多久（est），
 *      加上安全余量 sched_margin 得 need；
 *   3. 距目标时刻 ≤ need 时放行（RUN）并锁存，之后全速充电，到目标由原有逻辑停充。
 * 安全网：
 *   - 电量低于 sched_floor 不按住（先正常充，回到 floor+3 再按住）；
 *   - 到目标时刻无论如何都放行；总开关关闭 / 预约关闭 / 目标已到 都立刻放行；
 *   - 距目标时刻超出 sched_window 不介入（避免白天插电被按住一整天）；
 *   - 用户可用 data/sched_skip 跳过“本次”预约。
 * 与原有停充判定的关系：reason_sched 与 reason_battery / reason_temp 并列，
 * 只有所有原因都解除才恢复充电；温控停充优先级不变。
 */
enum {
    SCHED_OFF = 0,   /* 未开启 */
    SCHED_HOLD = 1,  /* 按住，等待放行时刻 */
    SCHED_RUN = 2,   /* 已放行，正在充向目标 */
    SCHED_FLOOR = 3, /* 电量低于保底值，先正常充 */
    SCHED_IDLE = 4,  /* 未插电 / 距目标时刻超出窗口，不介入 */
    SCHED_DONE = 5,  /* 已到目标电量，无需预约 */
    SCHED_SKIP = 6   /* 用户选择本次立即充电 */
};

/* 充电曲线：g_spp[i] = 从 i% 涨到 i+1% 所需秒数；g_tail_sec = 到 100% 后涓流收尾秒数。
 * 默认值偏保守（宁可早点开始），之后按真实充电数据做指数滑动平均自学习。 */
static int g_spp[101];
static int g_tail_sec;
static int g_curve_dirty = 0;
static time_t g_curve_saved = 0;

static void curve_defaults(void)
{
    for (int i = 0; i < 100; i++)
        g_spp[i] = (i < 50) ? 40 : (i < 80) ? 50 : (i < 90) ? 90 : 150;
    g_spp[100] = 0;
    g_tail_sec = 900;
}

static void curve_load(void)
{
    curve_defaults();
    FILE *f = fopen(g_curve_file, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (!strncmp(p, "spp=", 4)) {
            int tmp[100], n = 0;
            char *tok = strtok(p + 4, ",");
            while (tok && n < 100) { tmp[n++] = atoi(tok); tok = strtok(NULL, ","); }
            if (n == 100)
                for (int i = 0; i < 100; i++)
                    if (tmp[i] >= 5 && tmp[i] <= 1800) g_spp[i] = tmp[i];
        } else if (!strncmp(p, "tail=", 5)) {
            int v = atoi(p + 5);
            if (v >= 0 && v <= 7200) g_tail_sec = v;
        }
    }
    fclose(f);
}

static void curve_save(void)
{
    char tmp[400];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_curve_file);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "spp=");
    for (int i = 0; i < 100; i++) fprintf(f, "%s%d", i ? "," : "", g_spp[i]);
    fprintf(f, "\ntail=%d\n", g_tail_sec);
    if (fclose(f) == 0) rename(tmp, g_curve_file);
}

static void curve_maybe_save(time_t now, int force)
{
    if (!g_curve_dirty) return;
    if (!force && now - g_curve_saved < 600) return;
    curve_save();
    g_curve_dirty = 0;
    g_curve_saved = now;
}

static void ema_update(int *slot, long sample, long lo, long hi)
{
    if (sample < lo || sample > hi) return;
    *slot = (int)((*slot * 7L + sample * 3L) / 10);
    g_curve_dirty = 1;
}

/* 学习：只统计“正在充、且没被本守护按住”时相邻 1% 的间隔；
 * 刚开始/刚放行后的第一个间隔起点不确定，丢弃（valid=0）。 */
static struct { int level; time_t t; int valid; time_t at100; int tail_done; } g_lrn = { -1, 0, 0, 0, 0 };

static void learn_reset(void)
{
    g_lrn.level = -1; g_lrn.valid = 0; g_lrn.at100 = 0; g_lrn.tail_done = 0;
}

static void learn_step(int charging_now, int level, time_t now)
{
    if (!charging_now) { learn_reset(); return; }
    if (g_lrn.level < 0 || level < g_lrn.level || level > g_lrn.level + 1) {
        g_lrn.level = level; g_lrn.t = now; g_lrn.valid = 0;
        return;
    }
    if (level == g_lrn.level + 1) {
        if (g_lrn.valid) ema_update(&g_spp[g_lrn.level], (long)(now - g_lrn.t), 8, 1800);
        g_lrn.level = level; g_lrn.t = now; g_lrn.valid = 1;
        if (level >= 100 && !g_lrn.at100) g_lrn.at100 = now;
    }
}

/* 涓流收尾结束（状态变 Full，或充满即停判定成功）时调用，最多量一次 */
static void learn_tail(time_t now)
{
    if (g_lrn.at100 && !g_lrn.tail_done) {
        ema_update(&g_tail_sec, (long)(now - g_lrn.at100), 30, 7200);
        g_lrn.tail_done = 1;
    }
}

/* 从 level 充到 target 预计耗时（秒，不含余量） */
static long sched_estimate(int level, int target, int full_mode)
{
    long s = 0;
    if (level < 0) level = 0;
    for (int l = level; l < target && l < 100; l++) s += g_spp[l];
    if (full_mode) s += g_tail_sec;
    return s;
}

/* now 之后最近的一个 hh:mm（本地时间）。now 恰好等于目标时刻算已过，取次日。 */
static time_t sched_next_deadline(time_t now, int hh, int mm)
{
    struct tm t;
    localtime_r(&now, &t);
    t.tm_hour = hh; t.tm_min = mm; t.tm_sec = 0; t.tm_isdst = -1;
    time_t d = mktime(&t);
    if (d <= now) {
        t.tm_mday += 1; t.tm_isdst = -1;
        d = mktime(&t);
    }
    return d;
}

typedef struct {
    time_t cycle_deadline; /* 下面两个锁存所属的目标时刻，换了就清零 */
    time_t skip_deadline;  /* 用户选择跳过的那一次 */
    int run_latch;
    int floor_latch;
} sched_state_t;

typedef struct {
    int state;
    time_t deadline;
    time_t resume_at;      /* 预计放行时刻 */
    long est;              /* 预计充电耗时 */
    long need;             /* est + 余量 */
} sched_out_t;

/* 返回 1 = 需要按住（停充），0 = 不按住。纯函数（除 st 外无副作用），便于离线测试。
 * powered：当前是否插着电；was_held：上一轮是否正由本预约按住。
 * 按住期间部分机型会把 status 变成 Not charging 甚至 online=0，
 * 所以“已按住”时不再要求 powered，靠放行条件/目标时刻收尾。 */
static int sched_decide(const config_t *cfg, int level, int powered, int was_held,
                        time_t now, sched_state_t *st, sched_out_t *o)
{
    memset(o, 0, sizeof(*o));
    o->state = SCHED_OFF;
    if (!cfg->sched_switch) {
        st->run_latch = 0; st->floor_latch = 0; st->cycle_deadline = 0;
        return 0;
    }

    time_t dl = sched_next_deadline(now, cfg->sched_hour, cfg->sched_min);
    o->deadline = dl;
    if (dl != st->cycle_deadline) {
        st->cycle_deadline = dl; st->run_latch = 0; st->floor_latch = 0;
    }
    if (dl == st->skip_deadline) { o->state = SCHED_SKIP; return 0; }
    if ((long)(dl - now) > (long)cfg->sched_window * 3600) { o->state = SCHED_IDLE; return 0; }
    if (!powered && !was_held) { o->state = SCHED_IDLE; return 0; }

    int full = cfg->charge_full;
    /* power_stop=110（关闭电量停充）时目标按 100，涓流收尾靠余量兜 */
    int target = full ? 100 : (cfg->power_stop <= 100 ? cfg->power_stop : 100);
    if (level >= 100 || (!full && level >= target)) { o->state = SCHED_DONE; return 0; }

    if (cfg->sched_floor > 0) {
        if (level < cfg->sched_floor) st->floor_latch = 1;
        else if (level >= cfg->sched_floor + 3) st->floor_latch = 0;
        if (st->floor_latch && !st->run_latch) { o->state = SCHED_FLOOR; return 0; }
    }

    long est = sched_estimate(level, target, full);
    long need = est + (long)cfg->sched_margin * 60;
    o->est = est; o->need = need; o->resume_at = dl - need;
    if (st->run_latch || (long)(dl - now) <= need) {
        st->run_latch = 1; o->state = SCHED_RUN;
        return 0;
    }
    o->state = SCHED_HOLD;
    return 1;
}

/* ===================== 状态输出 ===================== */
static void write_status_kv(const battery_t *b, int stopped, int reason_battery, int reason_temp, int off,
                            int reason_sched, const sched_out_t *so, int sched_on, time_t now_ts)
{
    FILE *f = fopen(g_status_kv, "w");
    if (!f) return;
    time_t now = time(NULL);
    struct tm tmv; localtime_r(&now, &tmv);
    char tsbuf[32];
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d_%H:%M:%S", &tmv);
    fprintf(f,
        "level=%d\n"
        "temp=%d\n"
        "powered=%d\n"
        "stopped=%d\n"
        "reason_battery=%d\n"
        "reason_temp=%d\n"
        "off=%d\n"
        "updated_at=%s\n",
        b->level, b->temp_c, b->powered, stopped, reason_battery, reason_temp, off, tsbuf);
    /* 预约充满：时间一律写 epoch 秒，由 WebUI 按本地时区格式化 */
    long dl = 0, resume = 0, finish = 0;
    int st = 0;
    if (so && sched_on) {
        st = so->state;
        dl = (long)so->deadline;
        if (st == SCHED_HOLD) { resume = (long)so->resume_at; finish = resume + so->est; }
        else if (st == SCHED_RUN) { finish = (long)now_ts + so->est; }
    }
    fprintf(f,
        "sched_on=%d\n"
        "sched_state=%d\n"
        "sched_held=%d\n"
        "sched_deadline=%ld\n"
        "sched_resume=%ld\n"
        "sched_finish=%ld\n",
        sched_on ? 1 : 0, st, reason_sched, dl, resume, finish);
    fclose(f);
}

/* ===================== 恢复 / 重扫 子命令 ===================== */
static int cmd_restore(void)
{
    load_device_profile();
    load_list_switch();
    int n = 0;
    if (g_has_preferred && write_node(g_preferred.path, g_preferred.start) == 0) n++;
    if (g_has_mca && write_node(g_mca.path, g_mca.start) == 0) n++;
    for (int i = 0; i < g_switch_n; i++) {
        if (write_node(g_switches[i].path, g_switches[i].start) == 0) n++;
    }
    for (int i = 0; i < FALLBACK_N; i++) {
        write_node(g_fallback[i].path, g_fallback[i].start);
    }
    printf("charge_guard: restore 完成，已恢复 %d 个已知节点为开始充电状态\n", n);
    return 0;
}

/* ===================== 主循环 ===================== */
int main(int argc, char *argv[])
{
    /* ★ 修复：-v / --version 必须最先处理，否则 customize.sh 的
     *   `charge_guardd -v` 会把 -v 当成 <moddir> 启动，argc==2 进不了
     *   restore/rescan，直接进 for(;;) 主循环永不退出，安装脚本卡死。 */
    if (argc >= 2 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version"))) {
        printf("charge_guardd 1.2\n");
        return 0;
    }

    const char *moddir = (argc >= 2) ? argv[1] : MODULE_BASE;
    build_paths(moddir);

    if (argc >= 3 && !strcmp(argv[2], "restore")) {
        return cmd_restore();
    }
    if (argc >= 3 && !strcmp(argv[2], "rescan")) {
        g_switches_ready = 0;
        printf("charge_guard: 请改用 charge_guard/list_switch.sh 触发重新扫描\n");
        return 0;
    }

    /* 持久化的判定状态：常驻单进程直接用变量记，不用 shell 版那套标记文件 */
    int stopped = 0;
    int reason_battery = 0, reason_temp = 0;
    int trickle_count = 0;
    int was_powered = -1; /* -1=未知，用于 power_reset 检测“刚插上” */
    int reason_sched = 0;              /* 预约充满正在按住 */
    sched_state_t sst;
    memset(&sst, 0, sizeof(sst));
    sched_out_t sout;
    memset(&sout, 0, sizeof(sout));
    curve_load();

    int was_off = 0;
    for (;;) {
        if (file_exists(g_off_flag)) {
            /* 总开关关闭：不读任何电池节点，60 秒查一次开关状态。
             * 但进入 off 状态时要写一次 status.kv（off=1），否则 WebUI
             * 读到的永远是关闭前最后一次的旧数据，分不清"真的还在跑
             * 只是数值没变"和"进程其实压根没在读"。只在刚进入 off 时
             * 写一次，不必每 60 秒重复写。 */
            if (!was_off) {
                /* 总开关关闭时若还按着停充（含预约按住、电量/温度停充），必须先放行，
                 * 否则守护退出工作后节点会一直停在“停充”，插着电也不充。
                 * 放行失败则 was_off 保持 0，60 秒后重试。 */
                if (stopped) {
                    static config_t offcfg;
                    load_config(&offcfg);
                    g_switches_ready = 0;
                    ensure_switches_loaded(&offcfg);
                    if (try_write("start") == 0) {
                        stopped = 0; reason_battery = 0; reason_temp = 0; reason_sched = 0;
                    }
                    g_switches_ready = 0;
                    g_cfg_for_switches = NULL;
                }
                if (!stopped) {
                    battery_t b = {0};
                    read_battery_sysfs(&b);
                    write_status_kv(&b, 0, 0, 0, 1, 0, NULL, 0, time(NULL));
                    was_off = 1;
                }
            }
            sleep(60);
            continue;
        }
        was_off = 0;

        config_t cfg;
        load_config(&cfg);
        g_switches_ready = 0; /* 配置可能被 WebUI 改了自定义节点，每轮重新惰性加载一次判断依据 */

        battery_t b;
        read_battery_sysfs(&b);
        read_battery_dumpsys_fallback(&b);

        if (b.level < 0 || b.temp_c == -1000) {
            /* 读不到关键数据，不做任何停充/恢复判定，避免误动作 */
            sleep(10);
            continue;
        }

        if (b.powered && was_powered != 1 && cfg.power_reset) {
            /* 刚插上电且开启了自动拔插：先不处理，交由下面的停充/恢复流程
             * 自然完成一次“停-启”，无需额外模拟拔插动作，避免重复触发。 */
        }
        if (was_powered == 1 && !b.powered) curve_maybe_save(time(NULL), 1);
        was_powered = b.powered;

        time_t now_ts = time(NULL);

        /* 预约充满：先处理“本次跳过”请求，再算本轮是否需要按住 */
        if (file_exists(g_sched_skip)) {
            unlink(g_sched_skip);
            if (cfg.sched_switch)
                sst.skip_deadline = sched_next_deadline(now_ts, cfg.sched_hour, cfg.sched_min);
        }
        /* 学习充电曲线：仅统计“正在充、且没被按住”的数据 */
        if (b.level >= 100 && b.status_code == 5) learn_tail(now_ts);
        learn_step(b.powered && b.status_code == 2 && !stopped, b.level, now_ts);
        curve_maybe_save(now_ts, 0);

        int sched_hold = sched_decide(&cfg, b.level, b.powered, reason_sched, now_ts, &sst, &sout);

        int switch_stop_mode = 0;
        int cpu_trip = 0; /* 温控触发 */

        if (b.powered) {
            if (cfg.temperature_switch && cfg.temperature_switch_stop > cfg.temperature_switch_start
                && b.temp_c >= cfg.temperature_switch_stop) {
                cpu_trip = 1;
            }

            int battery_trip = 0;
            if (cfg.charge_full) {
                /* 充满即停：power_stop 数值本身不再作为停充点——哪怕高于它也继续充，
                 * 只在真到 100% 时用涓流电流判定"真充满"再停。power_stop 的数字
                 * 只在关闭充满即停时才生效（和 WebUI 提示文案保持一致）。 */
                if (b.level >= 100) {
                    if (b.status_code == 5) {
                        /* 状态已是 Full：充电 IC 已判定充满，直接停充（对齐原 shell 版 zg_charge_full：
                         * Full 时不拦截停充，只有“未 Full”才用涓流电流<100mA 连续 3 次判定）。
                         * 原来这里只清计数、不置 battery_trip，Full 状态下永远不会停充。 */
                        trickle_count = 0;
                        battery_trip = 1;
                    } else {
                        int cur = read_sysfs_int("/sys/class/power_supply/battery/current_now");
                        if (cur != INT_MIN) {
                            if (cur < 0) cur = -cur;
                            if (cur < 100000) trickle_count++; else trickle_count = 0;
                        }
                        if (trickle_count >= 3) { battery_trip = 1; trickle_count = 0; learn_tail(now_ts); }
                    }
                }
            } else if (cfg.power_stop <= 100 && cfg.power_stop > cfg.power_start && b.level >= cfg.power_stop) {
                battery_trip = 1;
            }
            if (battery_trip) switch_stop_mode = 1;

            if (switch_stop_mode || cpu_trip) {
                int first_stop = !stopped;
                if (!cpu_trip && !cfg.charge_full && first_stop && cfg.power_stop_time > 0) {
                    sleep((unsigned)cfg.power_stop_time);
                }
                ensure_switches_loaded(&cfg);
                if (try_write("stop") == 0) {
                    stopped = 1;
                    if (switch_stop_mode) reason_battery = 1;
                    if (cpu_trip) reason_temp = 1;
                } /* 写入失败：保持原状态，下一轮继续尝试 */
            }
        } else {
            /* 未插电：只清涓流计数（这个和"是否在充电"绑定，断电后没意义）。
             * stopped/reason_battery/reason_temp 不在这里清零——它们对应的是
             * sysfs 停充节点当前的真实状态，必须等下面 try_write("start") 真正
             * 成功之后才能清；否则会出现"软件以为已恢复，节点其实还锁在 stop"
             * 的状态错位——插上不充电、显示未充电但电量掉得极慢，根因就在这。
             * 保留状态后，下面这段恢复判定不管插不插电都会照常跑，等条件满足
             * 就把节点写回 start，行为对齐旧 shell 版本（power_switch 标记文件
             * 从不因为拔线就被清掉）。 */
            trickle_count = 0;
        }

        /* 预约按住 / 放行 */
        if (sched_hold) {
            if (stopped) {
                reason_sched = 1;                 /* 节点已被别的原因停着，并入即可 */
            } else {
                ensure_switches_loaded(&cfg);
                if (try_write("stop") == 0) { stopped = 1; reason_sched = 1; }
            }
        } else {
            reason_sched = 0;
        }

        if (stopped) {
            int temp_ready = 1, battery_ready = 1;
            if (reason_temp) {
                if (cfg.temperature_switch && b.temp_c > cfg.temperature_switch_start) temp_ready = 0;
            }
            if (reason_battery) {
                if (cfg.charge_full) {
                    /* 充满即停触发的停充：只要没到 100% 就允许恢复充电，
                     * 不受 power_start 限制——用户拔线后电量哪怕只往下掉了 1%
                     * 再插上，也该直接继续充，不必等它跌回 power_start 那么低 */
                    if (b.level >= 100) battery_ready = 0;
                } else if (b.level > cfg.power_start && cfg.power_stop <= 100 && cfg.power_stop > cfg.power_start) {
                    battery_ready = 0;
                }
            }
            if (sched_hold) battery_ready = 0;    /* 预约仍需按住，其他原因解除也不放行 */
            if (temp_ready && battery_ready) {
                ensure_switches_loaded(&cfg);
                if (try_write("start") == 0) {
                    stopped = 0; reason_battery = 0; reason_temp = 0; reason_sched = 0;
                }
            }
        }

        write_status_kv(&b, stopped, reason_battery, reason_temp, 0,
                        reason_sched, &sout, cfg.sched_switch, now_ts);

        /* 按住期间 status 常变成 Not charging（powered=0），不能按 30 秒慢轮询，
         * 否则放行时刻会晚最多 30 秒；按住期间固定 10 秒一轮 */
        sleep(reason_sched ? 10 : (b.powered ? 3 : 30));
    }
    return 0;
}