/*
 * charge_guard.c — Aegis 充电阈值守护 v1.6
 * v1.6 修复：充满即停不再受恢复电量(power_start)限制（99%插电即补电到100%）；
 *         预约保底电量补电改为 +10% 滞回锁存（低于保底一路充到 保底+10% 才停，
 *         修复 20%停/19%充 的抖动循环），补电优先于预约按住
 * v1.5 修复：sched_skip 只查不删导致"跳过本次预约"变成永久跳过；现按文件 mtime
 *         与上一次目标时刻比对，上一轮目标过后自动清除，恢复"只跳过一次"语义
 * v1.4 修复：滞回被预约分支击穿、预约抖动(锁存)、低温误判、restore 参数、日志膨胀、
 *         电流符号标定、拉黑多次确认、探测残留还原
 * 编译: aarch64-linux-android21-clang -static -O2 -o charge_guardd charge_guard.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>

#ifndef MODDIR
#define MODDIR "/data/adb/modules/aegis_power_matrix"
#endif
#ifndef SYSROOT
#define SYSROOT ""   /* 仅模拟器使用：把 /sys /proc 重定向到沙盒 */
#endif
#define CONF MODDIR "/charge_guard.conf"
#define DATADIR MODDIR "/charge_guard/data"
#define LIST_SWITCH DATADIR "/list_switch"
#define DEVICE_PROFILE DATADIR "/device.profile"
#define BAD_SWITCH DATADIR "/bad_switch"
#define LOG_FILE DATADIR "/log.log"
#define STATUS_KV DATADIR "/status.kv"
#define POWER_SWITCH_FLAG DATADIR "/power_switch"
#define BATTERY_SWITCH_FLAG DATADIR "/battery_switch"
#define TEMP_SWITCH_FLAG DATADIR "/temp_switch"
#define OFF_FLAG DATADIR "/off"
#define IS_POWERED_FLAG DATADIR "/is_powered"
#define LEAK_COUNT_FILE DATADIR "/leak_count"
#define ORIG_VALUES_FILE DATADIR "/orig_values"
#define NOW_C_FILE DATADIR "/now_c"
#define NO_NODE_LOGGED DATADIR "/no_node_logged"
#define SCHED_SKIP DATADIR "/sched_skip"
#define SCHED_HOLD DATADIR "/sched_hold"     /* 本次停充由预约按住造成 */
#define SCHED_LATCH DATADIR "/sched_latch"   /* 预约放行锁存(存 deadline) */
#define SCHED_FLOOR_BOOST DATADIR "/sched_floor_boost" /* 保底补电锁存(充到 floor+10 才停) */
#define CUR_SIGN_FILE DATADIR "/cur_sign"    /* 充电电流符号标定 */
#define STRIKE_FILE DATADIR "/strikes"       /* 验证失败计数 */
#define DEBUG_FLAG DATADIR "/debug"          /* 存在则输出 DEBUG 日志 */

#define MAX_NODES 96
#define MAX_PATH 256
#define MAX_VAL 64
#define MAX_LINE 1024

/* ── 配置 ── */
typedef struct {
    int charge_full;
    int power_reset;
    int power_stop;
    int power_start;
    int power_stop_time;
    int temperature_switch;
    int temperature_switch_stop;
    int temperature_switch_start;
    int sched_on;        /* 预约充电总开关（来自 sched_switch） */
    char sched_time[8];  /* HH:MM */
    int sched_margin;    /* 安全余量（分钟） */
    int sched_floor;     /* 保底电量 */
    int sched_window;    /* 生效窗口（小时） */
} Config;

/* ── 节点 ── */
typedef struct {
    char path[MAX_PATH];
    char start_val[MAX_VAL];
    char stop_val[MAX_VAL];
    int is_current_limit;
} SwitchNode;

static SwitchNode g_nodes[MAX_NODES];
static int g_node_count = 0;
static char g_pref_path[MAX_PATH] = {0};
static char g_pref_start[MAX_VAL] = {0};
static char g_pref_stop[MAX_VAL] = {0};
static int g_pref_valid = 0;

/* 日志去重 */
static char g_last_log[512] = {0};
static time_t g_last_log_time = 0;

/* ── 工具函数 ── */
static int read_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n-1]=='\n'||buf[n-1]=='\r'||buf[n-1]==' '||buf[n-1]=='\t'))
        buf[--n] = '\0';
    return 0;
}

static int write_file_raw(const char *path, const char *val) {
    chmod(path, 0644);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t len = strlen(val);
    ssize_t n = write(fd, val, len);
    close(fd);
    return (n == len) ? 0 : -1;
}

/* 写节点 + 回读校验 */
static int write_node(const char *path, const char *val) {
    if (write_file_raw(path, val) < 0) return -1;
    usleep(10000);
    char got[MAX_VAL];
    if (read_file(path, got, sizeof(got)) == 0) {
        if (strcmp(got, val) != 0) return -2; /* 回读不一致 */
    }
    return 0;
}

#define LOG_MAX_BYTES (256 * 1024)
static void log_msg(const char *level, const char *fmt, ...) {
    int is_debug = (strcmp(level, "DEBUG") == 0);
    if (is_debug && access(DEBUG_FLAG, F_OK) != 0) return;   /* 默认不写 DEBUG */

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (!is_debug) {
        time_t now = time(NULL);
        if (strcmp(msg, g_last_log) == 0 && (now - g_last_log_time) < 60) return;
        strncpy(g_last_log, msg, sizeof(g_last_log)-1);
        g_last_log_time = now;
    }

    /* 简单轮转：超过 256KB 则保留一份 .1 */
    struct stat st;
    if (stat(LOG_FILE, &st) == 0 && st.st_size > LOG_MAX_BYTES) {
        rename(LOG_FILE, LOG_FILE ".1");
    }

    FILE *fp = fopen(LOG_FILE, "a");
    if (!fp) return;
    time_t now2 = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H:%M:%S", localtime(&now2));
    fprintf(fp, "%s [%s] %s\n", ts, level, msg);
    fclose(fp);
}

static int file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

static void touch_file(const char *path) {
    int fd = open(path, O_WRONLY|O_CREAT, 0644);
    if (fd >= 0) close(fd);
}

static void rm_file(const char *path) {
    unlink(path);
}

static int read_int_file(const char *path, int def) {
    char buf[32];
    if (read_file(path, buf, sizeof(buf)) < 0) return def;
    return atoi(buf);
}

/* ── 假节点拉黑（7天过期） ── */
static int is_bad_switch(const char *path) {
    FILE *fp = fopen(BAD_SWITCH, "r");
    if (!fp) return 0;
    char line[MAX_LINE];
    time_t now = time(NULL);
    while (fgets(line, sizeof(line), fp)) {
        char p[MAX_PATH];
        long ts;
        if (sscanf(line, "%255s %ld", p, &ts) == 2) {
            if (strcmp(p, path) == 0 && (now - ts) < 604800) {
                fclose(fp);
                return 1;
            }
        }
    }
    fclose(fp);
    return 0;
}

static void blacklist_switch(const char *path) {
    /* 先清理过期条目 */
    FILE *fp = fopen(BAD_SWITCH, "r");
    char lines[MAX_NODES][MAX_LINE];
    int n = 0;
    time_t now = time(NULL);
    if (fp) {
        while (n < MAX_NODES && fgets(lines[n], MAX_LINE, fp)) {
            char p[MAX_PATH]; long ts;
            if (sscanf(lines[n], "%255s %ld", p, &ts) == 2) {
                if ((now - ts) < 604800) n++;
            }
        }
        fclose(fp);
    }
    /* 去重追加 */
    if (!is_bad_switch(path) && n < MAX_NODES) {
        snprintf(lines[n], MAX_LINE, "%s %ld\n", path, (long)now);
        n++;
    }
    fp = fopen(BAD_SWITCH, "w");
    if (!fp) return;
    for (int i = 0; i < n; i++) fputs(lines[i], fp);
    fclose(fp);
}

/* ── 验证失败计数：24h 内累计 STRIKE_LIMIT 次才拉黑，避免瞬态误封真节点 ── */
#define STRIKE_LIMIT 2
#define STRIKE_TTL 86400
static int add_strike(const char *path) {
    char lines[MAX_NODES][MAX_LINE];
    int n = 0, count = 1, found = 0;
    time_t now = time(NULL);
    FILE *fp = fopen(STRIKE_FILE, "r");
    if (fp) {
        while (n < MAX_NODES && fgets(lines[n], MAX_LINE, fp)) {
            char p[MAX_PATH]; int c; long ts;
            if (sscanf(lines[n], "%255s %d %ld", p, &c, &ts) != 3) continue;
            if ((now - ts) >= STRIKE_TTL) continue;
            if (strcmp(p, path) == 0) {
                c++; count = c; found = 1;
                snprintf(lines[n], MAX_LINE, "%s %d %ld\n", p, c, (long)now);
            }
            n++;
        }
        fclose(fp);
    }
    if (!found && n < MAX_NODES) {
        snprintf(lines[n], MAX_LINE, "%s 1 %ld\n", path, (long)now);
        n++;
    }
    fp = fopen(STRIKE_FILE, "w");
    if (fp) { for (int i = 0; i < n; i++) fputs(lines[i], fp); fclose(fp); }
    return count;
}

static void clear_strike(const char *path) {
    FILE *fp = fopen(STRIKE_FILE, "r");
    if (!fp) return;
    char lines[MAX_NODES][MAX_LINE];
    int n = 0, changed = 0;
    while (n < MAX_NODES && fgets(lines[n], MAX_LINE, fp)) {
        char p[MAX_PATH];
        if (sscanf(lines[n], "%255s", p) == 1 && strcmp(p, path) == 0) { changed = 1; continue; }
        n++;
    }
    fclose(fp);
    if (!changed) return;
    fp = fopen(STRIKE_FILE, "w");
    if (fp) { for (int i = 0; i < n; i++) fputs(lines[i], fp); fclose(fp); }
}

/* 验证失败：未到次数只记一笔，到次数才拉黑。返回 1=已拉黑 */
static int probe_failed(const char *path, const char *what) {
    int c = add_strike(path);
    if (c >= STRIKE_LIMIT) {
        blacklist_switch(path);
        log_msg("WARN", "%s 已拉黑(第%d次失败) %s", what, c, path);
        return 1;
    }
    log_msg("WARN", "%s 第%d次失败，暂不拉黑 %s", what, c, path);
    return 0;
}

/* ── 限流节点原值保存/恢复 ── */
static void save_orig_value(const char *path, const char *val) {
    FILE *fp = fopen(ORIG_VALUES_FILE, "r");
    char lines[MAX_NODES][MAX_LINE];
    int n = 0, found = 0;
    if (fp) {
        while (n < MAX_NODES && fgets(lines[n], MAX_LINE, fp)) {
            char p[MAX_PATH];
            if (sscanf(lines[n], "%255s", p) == 1 && strcmp(p, path) == 0) {
                snprintf(lines[n], MAX_LINE, "%s %s\n", path, val);
                found = 1;
            }
            n++;
        }
        fclose(fp);
    }
    if (!found && n < MAX_NODES) {
        snprintf(lines[n], MAX_LINE, "%s %s\n", path, val);
        n++;
    }
    fp = fopen(ORIG_VALUES_FILE, "w");
    if (!fp) return;
    for (int i = 0; i < n; i++) fputs(lines[i], fp);
    fclose(fp);
}

static int get_orig_value(const char *path, char *val, size_t size) {
    FILE *fp = fopen(ORIG_VALUES_FILE, "r");
    if (!fp) return -1;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        char p[MAX_PATH], v[MAX_VAL];
        if (sscanf(line, "%255s %63s", p, v) == 2 && strcmp(p, path) == 0) {
            strncpy(val, v, size-1);
            val[size-1] = '\0';
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);
    return -1;
}

/* ── 充电电流符号标定 ──
   各厂商 current_now 符号不一致：有的充电为负，有的充电为正。
   只在“未停充 + status=Charging + 电流足够大”时采样，未标定前不使用电流判据。 */
static int g_charge_sign = 0;   /* +1: 充电时电流为正; -1: 为负; 0: 未知 */
static void load_cur_sign(void) {
    int v = read_int_file(CUR_SIGN_FILE, 0);
    g_charge_sign = (v == 1 || v == -1) ? v : 0;
}
static void calibrate_cur_sign(const char *status, int current) {
    if (strcmp(status, "Charging") != 0) return;
    int a = current < 0 ? -current : current;
    if (a < 300000) return;                       /* <300mA 不采样 */
    int s = current > 0 ? 1 : -1;
    if (s == g_charge_sign) return;
    g_charge_sign = s;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", s);
    write_file_raw(CUR_SIGN_FILE, buf);
    log_msg("INFO", "电流符号标定：充电时 current_now %s", s > 0 ? "为正" : "为负");
}
/* 电流方向与“充电方向”相反 => 确实没在充 */
static int current_says_not_charging(int current) {
    if (g_charge_sign == 0) return 0;
    return ((long)current * g_charge_sign) < 0;
}

/* ── 真断电验证：轮询 status + current_now ── */
static int verify_stopped(int timeout_sec) {
    char status[64];
    for (int i = 0; i < timeout_sec; i++) {
        if (read_file(SYSROOT "/sys/class/power_supply/battery/status", status, sizeof(status)) == 0) {
            if (strcmp(status, "Not charging") == 0 ||
                strcmp(status, "Discharging") == 0 ||
                strcmp(status, "Full") == 0) {
                return 1;
            }
        }
        /* 部分机型 status 不变但电流转正（放电） */
        int cur = read_int_file(SYSROOT "/sys/class/power_supply/battery/current_now", 0);
        if (current_says_not_charging(cur)) return 1;
        sleep(1);
    }
    return 0;
}

/* ── 限流节点判定 ── */
static int is_current_limit_node(const char *path) {
    return strstr(path, "constant_charge_current_max") ||
           strstr(path, "current_max") ||
           strstr(path, "input_current_max") ||
           strstr(path, "charge_current") ||
           strstr(path, "fast_charge_current_max") ||
           strstr(path, "charge_control_limit");
}

/* ── 配置读取 ── */
static void read_config(Config *cfg) {
    /* 默认值 */
    cfg->charge_full = 0;
    cfg->power_reset = 0;
    cfg->power_stop = 80;
    cfg->power_start = 70;
    cfg->power_stop_time = 3;
    cfg->temperature_switch = 1;
    cfg->temperature_switch_stop = 60;
    cfg->temperature_switch_start = 50;
    cfg->sched_on = 0;
    strcpy(cfg->sched_time, "08:00");
    cfg->sched_margin = 15;
    cfg->sched_floor = 20;
    cfg->sched_window = 12;

    FILE *fp = fopen(CONF, "r");
    if (!fp) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64], val[64];
        if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
            if (strcmp(key, "charge_full") == 0) cfg->charge_full = atoi(val);
            else if (strcmp(key, "power_reset") == 0) cfg->power_reset = atoi(val);
            else if (strcmp(key, "power_stop") == 0) cfg->power_stop = atoi(val);
            else if (strcmp(key, "power_start") == 0) cfg->power_start = atoi(val);
            else if (strcmp(key, "power_stop_time") == 0) cfg->power_stop_time = atoi(val);
            else if (strcmp(key, "temperature_switch") == 0) cfg->temperature_switch = atoi(val);
            else if (strcmp(key, "temperature_switch_stop") == 0) cfg->temperature_switch_stop = atoi(val);
            else if (strcmp(key, "temperature_switch_start") == 0) cfg->temperature_switch_start = atoi(val);
            else if (strcmp(key, "sched_switch") == 0) cfg->sched_on = atoi(val);
            else if (strcmp(key, "sched_time") == 0) strncpy(cfg->sched_time, val, sizeof(cfg->sched_time)-1);
            else if (strcmp(key, "sched_margin") == 0) cfg->sched_margin = atoi(val);
            else if (strcmp(key, "sched_floor") == 0) cfg->sched_floor = atoi(val);
            else if (strcmp(key, "sched_window") == 0) cfg->sched_window = atoi(val);
        }
    }
    fclose(fp);

    /* 配置兜底 */
    if (cfg->power_stop > 100) cfg->power_stop = 100;
    if (cfg->power_stop < 1) cfg->power_stop = 1;
    if (cfg->power_start < 1) cfg->power_start = 1;
    if (cfg->power_start > 100) cfg->power_start = 100;
    if (cfg->power_stop <= cfg->power_start) {
        cfg->power_start = cfg->power_stop - 5;
        if (cfg->power_start < 1) cfg->power_start = 1;
    }
    if (cfg->power_stop_time < 1) cfg->power_stop_time = 1;
    if (cfg->power_stop_time > 120) cfg->power_stop_time = 120;
    if (cfg->temperature_switch_stop < 25) cfg->temperature_switch_stop = 25;
    if (cfg->temperature_switch_stop > 70) cfg->temperature_switch_stop = 70;
    if (cfg->temperature_switch_start < 25) cfg->temperature_switch_start = 25;
    if (cfg->temperature_switch_start > 70) cfg->temperature_switch_start = 70;
    if (cfg->temperature_switch_stop <= cfg->temperature_switch_start) {
        cfg->temperature_switch_start = cfg->temperature_switch_stop - 5;
        if (cfg->temperature_switch_start < 25) cfg->temperature_switch_start = 25;
    }
}

/* ── 电池状态读取 ── */
static int read_battery(int *level, int *temp, char *status, size_t st_size, int *current) {
    char buf[64];

    if (read_file(SYSROOT "/sys/class/power_supply/battery/capacity", buf, sizeof(buf)) < 0)
        return -1;
    *level = atoi(buf);

    if (read_file(SYSROOT "/sys/class/power_supply/battery/temp", buf, sizeof(buf)) == 0) {
        /* Android power_supply 的 temp 恒为 0.1°C。旧版 "t>200 才除10" 会把 4.5~20°C 误判成 45~200°C */
        *temp = atoi(buf) / 10;
    } else {
        *temp = 25;
    }

    if (read_file(SYSROOT "/sys/class/power_supply/battery/status", status, st_size) < 0)
        strcpy(status, "Unknown");

    *current = read_int_file(SYSROOT "/sys/class/power_supply/battery/current_now", 0);

    return 0;
}

/* ── 节点扫描（list_switch 的 C 实现） ── */
static void add_node(const char *path, const char *start, const char *stop) {
    if (g_node_count >= MAX_NODES) return;
    if (!file_exists(path)) return;

    /* 去重 */
    for (int i = 0; i < g_node_count; i++) {
        if (strcmp(g_nodes[i].path, path) == 0) return;
    }

    SwitchNode *n = &g_nodes[g_node_count];
    strncpy(n->path, path, MAX_PATH-1);
    strncpy(n->start_val, start, MAX_VAL-1);
    strncpy(n->stop_val, stop, MAX_VAL-1);
    n->is_current_limit = is_current_limit_node(path);
    g_node_count++;
}

static void scan_nodes(void) {
    g_node_count = 0;

    /* 先读 list_switch（如果存在） */
    FILE *fp = fopen(LIST_SWITCH, "r");
    if (fp) {
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), fp) && g_node_count < MAX_NODES) {
            char path[MAX_PATH], sv[MAX_VAL], sp[MAX_VAL];
            /* 格式: path,start=X,stop=Y */
            if (sscanf(line, "%255[^,],start=%63[^,],stop=%63[^\r\n]", path, sv, sp) == 3) {
                /* shell 版 list_switch 用 _ 代替空格 */
                for (char *c = sv; *c; c++) if (*c == '_') *c = ' ';
                for (char *c = sp; *c; c++) if (*c == '_') *c = ' ';
                add_node(path, sv, sp);
            }
        }
        fclose(fp);
    }

    /* 内置兜底列表（与 shell 版一致） */
    static const struct { const char *p, *s, *t; } fallback[] = {
        {SYSROOT "/sys/class/power_supply/battery/charge_charger_state","1","0"},
        {SYSROOT "/sys/class/power_supply/battery/connect_disable","0","1"},
        {SYSROOT "/sys/class/power_supply/battery/batt_slate_mode","0","1"},
        {SYSROOT "/sys/class/power_supply/battery/store_mode","0","1"},
        {SYSROOT "/sys/class/power_supply/battery/input_suspend","0","1"},
        {SYSROOT "/sys/class/power_supply/battery/battery_input_suspend","0","1"},
        {SYSROOT "/sys/class/power_supply/battery/night_charging","0","1"},
        {SYSROOT "/sys/class/power_supply/battery/charging_enabled","1","0"},
        {SYSROOT "/sys/class/power_supply/battery/battery_charging_enabled","1","0"},
        {SYSROOT "/sys/class/power_supply/battery/batt_charging_enabled","1","0"},
        {SYSROOT "/sys/class/power_supply/battery/charge_disable","0","1"},
        {SYSROOT "/sys/class/power_supply/battery/disable_charging","0","1"},
        {SYSROOT "/sys/class/power_supply/battery/stop_charging","0","1"},
        {SYSROOT "/sys/class/power_supply/battery/charge_enabled","1","0"},
        {SYSROOT "/sys/class/power_supply/battery/op_disable_charge","0","1"},
        {SYSROOT "/sys/class/power_supply/battery/device/Charging_Enable","1","0"},
        {SYSROOT "/sys/class/power_supply/battery/ChargerEnable","1","0"},
        {SYSROOT "/sys/class/power_supply/battery/force_disable_charging","0","1"},
        {SYSROOT "/sys/class/power_supply/battery/charge_control_enabled","1","0"},
        {SYSROOT "/sys/class/power_supply/battery/mi_charge_enable","1","0"},
        {SYSROOT "/sys/class/power_supply/battery_ext/smart_charging_interruption","0","1"},
        {SYSROOT "/sys/class/power_supply/main/adapter_cc_mode","0","1"},
        {SYSROOT "/sys/class/power_supply/main/cool_mode","0","1"},
        {SYSROOT "/sys/class/power_supply/charger/charge_disable","0","1"},
        {SYSROOT "/sys/class/power_supply/bms/charge_disable","0","1"},
        {SYSROOT "/sys/class/power_supply/bms/charging_enabled","1","0"},
        {SYSROOT "/sys/class/power_supply/bms/charge_enabled","1","0"},
        {SYSROOT "/sys/class/power_supply/mi_chg/charge_disable","0","1"},
        {SYSROOT "/sys/class/power_supply/mi_chg/charging_enabled","1","0"},
        {SYSROOT "/sys/class/qcom-battery/charging_enabled","1","0"},
        {SYSROOT "/sys/class/qcom-battery/charge_disable","0","1"},
        {SYSROOT "/sys/class/qcom-battery/input_suspend","0","1"},
        {SYSROOT "/sys/class/qcom-battery/battery_charging_enabled","1","0"},
        {SYSROOT "/sys/class/qcom-battery/charging_suspend_battery","0","1"},
        {SYSROOT "/sys/class/qcom-battery/night_charging","0","1"},
        {SYSROOT "/sys/class/qcom-battery/cool_mode","0","1"},
        {SYSROOT "/sys/class/qcom-battery/batt_protect_en","0","1"},
        {SYSROOT "/sys/class/asuslib/charger_limit_en","0","1"},
        {SYSROOT "/sys/class/asuslib/charging_suspend_en","0","1"},
        {SYSROOT "/sys/class/hw_power/charger/charge_data/enable_charger","1","0"},
        {SYSROOT "/sys/class/power_supply/idt/pin_enabled","1","0"},
        {SYSROOT "/sys/devices/platform/huawei_charger/enable_charger","1","0"},
        {SYSROOT "/sys/devices/platform/lge-unified-nodes/charging_enable","1","0"},
        {SYSROOT "/sys/devices/platform/lge-unified-nodes/charging_completed","0","1"},
        {SYSROOT "/sys/devices/platform/mt-battery/disable_charger","0","1"},
        {SYSROOT "/sys/devices/platform/battery/ChargerEnable","1","0"},
        {SYSROOT "/sys/devices/platform/soc/soc:google,charger/charge_disable","0","1"},
        {SYSROOT "/sys/devices/platform/charger/bypass_charger","0","1"},
        {SYSROOT "/sys/devices/platform/soc/soc:qcom,pmic_glink/soc:qcom,pmic_glink:qcom,battery_charger/force_charger_suspend","0","1"},
        {SYSROOT "/sys/devices/virtual/oplus_chg/battery/mmi_charging_enable","1","0"},
        {SYSROOT "/sys/kernel/debug/google_charger/chg_suspend","0","1"},
        {SYSROOT "/sys/kernel/debug/google_charger/input_suspend","0","1"},
        {SYSROOT "/sys/kernel/debug/google_charger/chg_mode","1","0"},
        {SYSROOT "/sys/kernel/nubia_charge/charger_bypass","off","on"},
        {SYSROOT "/sys/module/qpnp_adaptive_charge/parameters/blocking","0","1"},
        {SYSROOT "/proc/driver/charger_limit_enable","0","1"},
        {SYSROOT "/proc/driver/charger_limit","100","1"},
        {SYSROOT "/proc/mtk_battery_cmd/current_cmd","0 0","0 1"},
        {SYSROOT "/proc/mtk_battery_cmd/en_power_path","1","0"},
        {SYSROOT "/sys/class/power_supply/battery/constant_charge_current_max","3000000","0"},
        {SYSROOT "/sys/class/power_supply/battery/current_max","3000000","0"},
        {SYSROOT "/sys/class/power_supply/battery/input_current_max","3000000","0"},
        {SYSROOT "/sys/class/power_supply/battery/charge_current","3000000","0"},
        {SYSROOT "/sys/class/power_supply/battery/fast_charge_current_max","3000000","0"},
        {SYSROOT "/sys/class/power_supply/usb/input_suspend","0","1"},
        {SYSROOT "/sys/class/power_supply/qc_usb/input_suspend","0","1"},
        {SYSROOT "/sys/class/power_supply/dc/input_suspend","0","1"},
        {SYSROOT "/sys/class/power_supply/ac/device/power_supply/usb/power_switch","1","0"},
        {SYSROOT "/sys/class/power_supply/ac/device/power_supply/usb/input_suspend","0","1"},
        {SYSROOT "/sys/class/power_supply/battery/charge_control_end_threshold","100","0"},
        {SYSROOT "/sys/class/power_supply/battery/charge_type","Fast","None"},
        {SYSROOT "/sys/class/power_supply/pc_port/input_suspend","0","1"},
        {SYSROOT "/sys/class/power_supply/wireless/input_suspend","0","1"},
    };
    for (size_t i = 0; i < sizeof(fallback)/sizeof(fallback[0]); i++) {
        add_node(fallback[i].p, fallback[i].s, fallback[i].t);
    }
}

/* ── preferred 管理 ── */
static void load_preferred(void) {
    g_pref_valid = 0;
    FILE *fp = fopen(DEVICE_PROFILE, "r");
    if (!fp) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        char key[64], val[MAX_PATH];
        if (sscanf(line, "%63[^=]=%255s", key, val) == 2) {
            if (strcmp(key, "preferred_switch") == 0) {
                strncpy(g_pref_path, val, MAX_PATH-1);
            } else if (strcmp(key, "preferred_switch_start") == 0) {
                strncpy(g_pref_start, val, MAX_VAL-1);
            } else if (strcmp(key, "preferred_switch_stop") == 0) {
                strncpy(g_pref_stop, val, MAX_VAL-1);
            }
        }
    }
    fclose(fp);
    if (g_pref_path[0] && g_pref_start[0] && g_pref_stop[0]) {
        g_pref_valid = 1;
    }
}

static void save_preferred(const char *path, const char *start, const char *stop) {
    FILE *fp = fopen(DEVICE_PROFILE, "r");
    char lines[64][MAX_LINE];
    int n = 0;
    if (fp) {
        while (n < 64 && fgets(lines[n], MAX_LINE, fp)) n++;
        fclose(fp);
    }

    fp = fopen(DEVICE_PROFILE, "w");
    if (!fp) return;

    int found_path = 0, found_start = 0, found_stop = 0;
    for (int i = 0; i < n; i++) {
        if (strncmp(lines[i], "preferred_switch=", 17) == 0) {
            fprintf(fp, "preferred_switch=%s\n", path);
            found_path = 1;
        } else if (strncmp(lines[i], "preferred_switch_start=", 23) == 0) {
            fprintf(fp, "preferred_switch_start=%s\n", start);
            found_start = 1;
        } else if (strncmp(lines[i], "preferred_switch_stop=", 22) == 0) {
            fprintf(fp, "preferred_switch_stop=%s\n", stop);
            found_stop = 1;
        } else {
            fputs(lines[i], fp);
        }
    }
    if (!found_path) fprintf(fp, "preferred_switch=%s\n", path);
    if (!found_start) fprintf(fp, "preferred_switch_start=%s\n", start);
    if (!found_stop) fprintf(fp, "preferred_switch_stop=%s\n", stop);
    fclose(fp);
}

static void clear_preferred(void) {
    save_preferred("", "", "");
    g_pref_valid = 0;
}

/* 限流节点恢复值：优先用停充前保存的原值，而不是硬编码 3000000 */
static const char *resolve_start(const char *path, const char *dflt, char *buf, size_t sz) {
    if (is_current_limit_node(path) && get_orig_value(path, buf, sz) == 0) return buf;
    return dflt;
}

/* 探测/使用一个节点停充。成功返回 1；失败一律还原为写前原值（含回读不一致的 -2） */
static int probe_node(const char *path, const char *stop_val, const char *start_val,
                      int is_limit, int is_pref, char *used, size_t used_size) {
    char orig[MAX_VAL] = {0};
    int have_orig = (read_file(path, orig, sizeof(orig)) == 0);
    /* 已处于 stop 值时不要把它当“原值”保存，否则会把 0 存成原值，之后永远恢复不了 */
    if (have_orig && strcmp(orig, stop_val) == 0) have_orig = 0;
    if (is_limit && have_orig) save_orig_value(path, orig);

    int rc = write_node(path, stop_val);
    if (rc == 0 && verify_stopped(5)) {
        snprintf(used, used_size, "%s=%s%s", path, stop_val, is_pref ? " (preferred)" : "");
        clear_strike(path);
        return 1;
    }
    if (rc != -1) {                                   /* -1 = 打不开，没改动 */
        write_file_raw(path, have_orig ? orig : start_val);
    }
    if (rc == 0) probe_failed(path, is_limit ? "假节点（限流型）" : "假节点（写入成功但未断电）");
    return 0;
}

/* ── 停充：遍历节点，验证真断电 ── */
static int do_power_stop(char *used_node, size_t node_size) {
    char skip[MAX_PATH] = {0};

    /* 优先 preferred */
    if (g_pref_valid && !is_bad_switch(g_pref_path) && file_exists(g_pref_path)) {
        char pp[MAX_PATH];
        strncpy(pp, g_pref_path, MAX_PATH - 1); pp[MAX_PATH - 1] = '\0';
        if (probe_node(pp, g_pref_stop, g_pref_start, is_current_limit_node(pp), 1,
                       used_node, node_size)) return 1;
        strncpy(skip, pp, MAX_PATH - 1);
        if (is_bad_switch(pp)) {            /* 累计失败达到上限才清除 preferred */
            clear_preferred();
            log_msg("WARN", "preferred 节点验证失败已清除 %s", pp);
        }
    }

    /* 限流节点排最后 */
    SwitchNode *normal[MAX_NODES], *limit[MAX_NODES];
    int n_normal = 0, n_limit = 0;
    for (int i = 0; i < g_node_count; i++) {
        if (is_bad_switch(g_nodes[i].path)) continue;
        if (!file_exists(g_nodes[i].path)) continue;
        if (skip[0] && strcmp(g_nodes[i].path, skip) == 0) continue;
        if (g_nodes[i].is_current_limit) limit[n_limit++] = &g_nodes[i];
        else normal[n_normal++] = &g_nodes[i];
    }

    for (int pass = 0; pass < 2; pass++) {
        SwitchNode **list = pass == 0 ? normal : limit;
        int cnt = pass == 0 ? n_normal : n_limit;
        for (int i = 0; i < cnt; i++) {
            SwitchNode *n = list[i];
            if (probe_node(n->path, n->stop_val, n->start_val, n->is_current_limit, 0,
                           used_node, node_size)) {
                save_preferred(n->path, n->start_val, n->stop_val);
                load_preferred();
                return 1;
            }
        }
    }
    return 0;
}

/* ── 恢复充电 ── */
static int do_power_start(char *used_node, size_t node_size, char *used_val, size_t val_size) {
    char buf[MAX_VAL];

    /* 优先 preferred */
    if (g_pref_valid && file_exists(g_pref_path)) {
        const char *v = resolve_start(g_pref_path, g_pref_start, buf, sizeof(buf));
        if (write_node(g_pref_path, v) == 0) {
            snprintf(used_node, node_size, "%s", g_pref_path);
            snprintf(used_val, val_size, "%s", v);
            return 1;
        }
    }

    /* 第一轮：只动“当前确实处于 stop 值”的节点，避免误写无关节点后误报成功 */
    int ok = 0;
    for (int i = 0; i < g_node_count; i++) {
        SwitchNode *n = &g_nodes[i];
        if (!file_exists(n->path)) continue;
        char cur[MAX_VAL];
        if (read_file(n->path, cur, sizeof(cur)) != 0 || strcmp(cur, n->stop_val) != 0) continue;
        const char *v = resolve_start(n->path, n->start_val, buf, sizeof(buf));
        if (write_node(n->path, v) == 0 && !ok) {
            snprintf(used_node, node_size, "%s", n->path);
            snprintf(used_val, val_size, "%s", v);
            ok = 1;
        }
    }
    if (ok) return 1;

    /* 第二轮：兜底，沿用旧行为（跳过黑名单） */
    for (int i = 0; i < g_node_count; i++) {
        SwitchNode *n = &g_nodes[i];
        if (!file_exists(n->path)) continue;
        if (is_bad_switch(n->path)) continue;
        const char *v = resolve_start(n->path, n->start_val, buf, sizeof(buf));
        if (write_node(n->path, v) == 0) {
            snprintf(used_node, node_size, "%s", n->path);
            snprintf(used_val, val_size, "%s", v);
            return 1;
        }
    }
    return 0;
}

/* ── 预约充电逻辑 ── */
/* 状态定义：
   0=待命 1=已按住 2=已放行 3=保底直充 5=已到目标 6=已跳过 */

/* 解析 HH:MM 为当天时间戳 */
static time_t parse_sched_time(const char *time_str) {
    int hour = 8, min = 0;
    if (sscanf(time_str, "%d:%d", &hour, &min) != 2) {
        hour = 8; min = 0;
    }
    if (hour < 0 || hour > 23) hour = 8;
    if (min < 0 || min > 59) min = 0;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    tm->tm_hour = hour;
    tm->tm_min = min;
    tm->tm_sec = 0;
    time_t target = mktime(tm);

    /* 如果目标时间已过，设为明天 */
    if (target <= now) {
        tm->tm_mday += 1;
        target = mktime(tm);
    }
    return target;
}

/* 估算充电时长（分钟）：简化模型，假设每分钟充1% */
static int estimate_charge_minutes(int current_level, int target_level) {
    if (current_level >= target_level) return 0;
    /* 实际充电速度受温度、电流等影响，这里用保守估计 */
    return (target_level - current_level) * 2; /* 每分钟0.5%，2分钟1% */
}

/* 预约放行锁存：放行后不再回到“按住”，避免 start_time 随电量漂移造成抖动 */
static long read_latch(void) {
    FILE *fp = fopen(SCHED_LATCH, "r");
    long v = 0;
    if (fp) { if (fscanf(fp, "%ld", &v) != 1) v = 0; fclose(fp); }
    return v;
}
static void write_latch(long v) {
    FILE *fp = fopen(SCHED_LATCH, "w");
    if (fp) { fprintf(fp, "%ld\n", v); fclose(fp); }
}

/* 预约充电判定：返回是否需要停充 */
static int sched_should_stop(int level, int powered, const Config *cfg,
                             int *state, time_t *deadline, time_t *resume, time_t *finish) {
    (void)powered;
    *state = 0;
    *deadline = 0;
    *resume = 0;
    *finish = 0;

    if (!cfg->sched_on) {
        rm_file(SCHED_FLOOR_BOOST);
        return 0;
    }

    time_t now = time(NULL);
    time_t target_time = parse_sched_time(cfg->sched_time);
    /* 目标电量：charge_full=1 为 100，否则为 power_stop（与配置文件注释一致） */
    int target_level = cfg->charge_full ? 100 : cfg->power_stop;
    int charge_min = estimate_charge_minutes(level, target_level);
    time_t start_time = target_time - (cfg->sched_margin * 60) - (charge_min * 60);
    time_t window_start = target_time - (cfg->sched_window * 3600);

    *deadline = target_time;
    *resume = start_time;
    *finish = target_time;

    /* v1.5：跳过只对一轮目标生效。parse_sched_time 恒返回未来时间，
       上一次目标时刻 = target_time - 86400；skip 文件 mtime 早于它，
       说明被跳过的那一轮已结束，自动清除，避免永久跳过 */
    if (file_exists(SCHED_SKIP)) {
        struct stat st;
        if (stat(SCHED_SKIP, &st) == 0 && st.st_mtime < target_time - 86400) {
            rm_file(SCHED_SKIP);
            log_msg("INFO", "上一轮预约已结束，自动清除跳过标记 sched_skip");
        } else {
            *state = 6; /* 已跳过 */
            return 0;
        }
    }

    /* 窗口外：不干预，同时清掉上一轮遗留的保底补电锁存 */
    if (now < window_start || now > target_time + 3600) {
        rm_file(SCHED_FLOOR_BOOST);
        *state = 0;
        return 0;
    }

    /* 已到目标时间且电量满：停充（注：parse_sched_time 恒返回未来时间，此分支实际不可达，保留兼容） */
    if (now >= target_time && level >= 100) {
        *state = 5;
        return 1;
    }

    /* v1.6 保底电量补电（+10% 滞回）：
       电量低于保底 → 写入补电锁存，一路充到 保底+10% 才允许停；
       修复旧版充到保底就停、掉 1% 又充的抖动（20%停/19%充循环）。
       补电优先于预约按住，无论距预约放行时刻还有多久。 */
    if (cfg->sched_floor > 0) {
        int in_boost = file_exists(SCHED_FLOOR_BOOST);
        if (!in_boost && level < cfg->sched_floor) {
            touch_file(SCHED_FLOOR_BOOST);
            in_boost = 1;
            log_msg("INFO", "预约充电：电量%d%% 低于保底%d%%，补电至%d%%",
                    level, cfg->sched_floor, cfg->sched_floor + 10);
        }
        if (in_boost) {
            if (level < cfg->sched_floor + 10 && level < 100) {
                *state = 3; /* 保底补电中，持续直充 */
                return 0;
            }
            /* 已补到保底+10%：清除锁存，交回预约按住逻辑 */
            rm_file(SCHED_FLOOR_BOOST);
            log_msg("INFO", "预约充电：保底补电完成（%d%%），恢复预约按住", level);
        }
    }

    /* 到点放行，或已锁存放行 */
    if (now >= start_time || read_latch() == (long)target_time) {
        if (read_latch() != (long)target_time) write_latch((long)target_time);
        *state = 2;
        return 0;
    }

    *state = 1; /* 按住 */
    return 1;
}

/* ── 写 status.kv（内容变化或每 60 秒才落盘，减少 flash 写入） ── */
static void write_status_kv(int level, int temp, int powered, int stopped,
                            int reason_battery, int reason_temp, int off,
                            const char *bat_status, int verified, int leak,
                            const char *node,
                            int sched_on, int sched_state, long sched_deadline,
                            long sched_resume, long sched_finish, int sched_held) {
    static char last_body[1024];
    static time_t last_write = 0;
    char body[1024];
    snprintf(body, sizeof(body),
             "level=%d\ntemp=%d\npowered=%d\nstopped=%d\nreason_battery=%d\nreason_temp=%d\n"
             "off=%d\nbat_status=%s\nverified=%d\nleak=%d\nnode=%s\nsched_on=%d\nsched_state=%d\n"
             "sched_deadline=%ld\nsched_resume=%ld\nsched_finish=%ld\nsched_held=%d\n",
             level, temp, powered, stopped, reason_battery, reason_temp, off, bat_status,
             verified, leak, node, sched_on, sched_state, sched_deadline, sched_resume,
             sched_finish, sched_held);
    time_t now = time(NULL);
    if (strcmp(body, last_body) == 0 && (now - last_write) < 60) return;

    FILE *fp = fopen(STATUS_KV, "w");
    if (!fp) return;
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H:%M:%S", localtime(&now));
    fputs(body, fp);
    fprintf(fp, "updated_at=%s\n", ts);
    fclose(fp);
    strncpy(last_body, body, sizeof(last_body) - 1);
    last_write = now;
}

/* 把所有处于“停充值”的节点还原；启动自检与 restore 子命令共用 */
static int restore_all_nodes(void) {
    int fixed = 0;
    scan_nodes();
    load_preferred();
    for (int i = 0; i < g_node_count; i++) {
        char cur[MAX_VAL], buf[MAX_VAL];
        if (read_file(g_nodes[i].path, cur, sizeof(cur)) != 0) continue;
        if (strcmp(cur, g_nodes[i].stop_val) != 0) continue;
        const char *rv = resolve_start(g_nodes[i].path, g_nodes[i].start_val, buf, sizeof(buf));
        write_node(g_nodes[i].path, rv);
        log_msg("INFO", "恢复节点 %s <- %s", g_nodes[i].path, rv);
        fixed++;
    }
    return fixed;
}

/* ── 主逻辑 ── */
int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        printf("charge_guardd v1.6 (Aegis)\n");
        return 0;
    }
    mkdir(DATADIR, 0755);

    /* restore 子命令：uninstall.sh 以 `charge_guardd <MODDIR> restore` 调用。
       旧版会把它当成启动守护进程，导致卸载脚本卡死。 */
    if ((argc > 1 && strcmp(argv[1], "restore") == 0) ||
        (argc > 2 && strcmp(argv[2], "restore") == 0)) {
        int n = restore_all_nodes();
        rm_file(POWER_SWITCH_FLAG); rm_file(BATTERY_SWITCH_FLAG); rm_file(TEMP_SWITCH_FLAG);
        rm_file(LEAK_COUNT_FILE); rm_file(IS_POWERED_FLAG); rm_file(SCHED_HOLD); rm_file(SCHED_LATCH);
        rm_file(SCHED_FLOOR_BOOST);
        log_msg("INFO", "restore：已还原 %d 个节点", n);
        printf("restored %d node(s)\n", n);
        return 0;
    }
    load_cur_sign();

    /* 首次运行：扫描节点 */
    if (!file_exists(LIST_SWITCH)) {
        scan_nodes();
        FILE *fp = fopen(LIST_SWITCH, "w");
        if (fp) {
            for (int i = 0; i < g_node_count; i++) {
                fprintf(fp, "%s,start=%s,stop=%s\n",
                        g_nodes[i].path, g_nodes[i].start_val, g_nodes[i].stop_val);
            }
            fclose(fp);
        }
    }

    log_msg("INFO", "charge_guard v1.6 启动");

    /* 启动时状态一致性检查：节点是停充状态但 flag 不存在，则恢复节点 */
    if (!file_exists(POWER_SWITCH_FLAG)) restore_all_nodes();

    int was_powered = 0;
    int no_node_logged = 0;

    while (1) {
        /* 总开关检查：关闭时恢复充电 */
        if (file_exists(OFF_FLAG)) {
            rm_file(IS_POWERED_FLAG);
            was_powered = 0;
            if (file_exists(POWER_SWITCH_FLAG)) {
                char used[MAX_LINE], val[MAX_VAL];
                do_power_start(used, sizeof(used), val, sizeof(val));
                rm_file(POWER_SWITCH_FLAG);
                rm_file(BATTERY_SWITCH_FLAG);
                rm_file(TEMP_SWITCH_FLAG);
                rm_file(LEAK_COUNT_FILE);
                log_msg("INFO", "总开关关闭，恢复充电 [%s <- %s]", used, val);
            }
            /* M5: off 状态也写 status.kv，让 WebUI 显示 off=1 */
            int level, temp, current;
            char status[64];
            if (read_battery(&level, &temp, status, sizeof(status), &current) == 0) {
                write_status_kv(level, temp, 0, 0, 0, 0, 1, status, 0, 0, "",
                                0, 0, 0, 0, 0, 0);
            }
            sleep(3);
            continue;
        }

        Config cfg;
        read_config(&cfg);

        int level, temp, current;
        char status[64];
        if (read_battery(&level, &temp, status, sizeof(status), &current) < 0) {
            log_msg("ERROR", "无法读取电池状态");
            sleep(3);
            continue;
        }

        if (!file_exists(POWER_SWITCH_FLAG)) calibrate_cur_sign(status, current);
        int powered = (strcmp(status, "Charging") == 0 || strcmp(status, "Full") == 0);
        if (powered) touch_file(IS_POWERED_FLAG);
        else rm_file(IS_POWERED_FLAG);

        /* 加载 preferred 和节点列表 */
        load_preferred();
        scan_nodes();

        int stopped = file_exists(POWER_SWITCH_FLAG);
        int reason_battery = file_exists(BATTERY_SWITCH_FLAG);
        int reason_temp = file_exists(TEMP_SWITCH_FLAG);
        int verified = 0, leak = 0;

        /* 检查是否真断电 */
        if (stopped) {
            if (strcmp(status, "Not charging") == 0 ||
                strcmp(status, "Discharging") == 0 ||
                strcmp(status, "Full") == 0 ||
                current_says_not_charging(current)) {
                verified = 1;
                rm_file(LEAK_COUNT_FILE);
            } else {
                leak = 1;
            }
        }

        /* ── 停充期自愈（v1.3） ── */
        if (stopped && leak) {
            int leak_cnt = read_int_file(LEAK_COUNT_FILE, 0);
            leak_cnt++;
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", leak_cnt);
            write_file_raw(LEAK_COUNT_FILE, buf);

            log_msg("WARN", "停充后仍在充电（漏充第%d轮）status=%s", leak_cnt, status);

            if (leak_cnt < 2) {
                /* 重申 */
                char used[MAX_LINE];
                if (do_power_stop(used, sizeof(used))) {
                    log_msg("INFO", "重申停充成功 [%s]", used);
                }
            } else {
                /* 清除 preferred 重新探测 */
                clear_preferred();
                rm_file(LEAK_COUNT_FILE);
                log_msg("WARN", "连续漏充，清除 preferred 重新探测");
                char used[MAX_LINE];
                if (do_power_stop(used, sizeof(used))) {
                    log_msg("INFO", "换节点后停充成功 [%s]", used);
                } else {
                    log_msg("ERROR", "换节点后仍无法停充");
                }
            }
        }

        /* ── 停充判定 ── */
        int need_stop = 0;
        int stop_by_battery = 0, stop_by_temp = 0;

        if (powered) {
            /* charge_full=1：只有100%涓流结束才停；否则按power_stop停 */
            if (cfg.charge_full) {
                static int low_current_count = 0;
                if (level >= 100) {
                    if (strcmp(status, "Full") == 0) {
                        /* 直接报 Full 的机型：立即停充 */
                        rm_file(NOW_C_FILE);
                        low_current_count = 0;
                        need_stop = 1;
                        stop_by_battery = 1;
                    } else {
                        int abs_current = current < 0 ? -current : current;
                        if (abs_current < 100000) {
                            low_current_count++;
                        } else {
                            low_current_count = 0;
                        }
                        if (low_current_count >= 3) {
                            need_stop = 1;
                            stop_by_battery = 1;
                        }
                    }
                } else {
                    /* 电量<100 时清零计数器 */
                    low_current_count = 0;
                }
                /* charge_full=1 且 level<100：不停充，忽略 power_stop */
            } else if (level >= cfg.power_stop) {
                need_stop = 1;
                stop_by_battery = 1;
            }

            if (cfg.temperature_switch && temp >= cfg.temperature_switch_stop) {
                need_stop = 1;
                stop_by_temp = 1;
            }

            if (need_stop && !stopped) {
                /* 延时 */
                if (stop_by_battery && !cfg.charge_full) {
                    log_msg("DEBUG", "电量%d 延时 %d 秒", level, cfg.power_stop_time);
                    sleep(cfg.power_stop_time);
                }

                char used[MAX_LINE];
                if (do_power_stop(used, sizeof(used))) {
                    touch_file(POWER_SWITCH_FLAG);
                    if (stop_by_battery) touch_file(BATTERY_SWITCH_FLAG);
                    if (stop_by_temp) touch_file(TEMP_SWITCH_FLAG);
                    log_msg("INFO", "电量%d 停止充电 [%s]", level, used);
                    no_node_logged = 0;
                } else if (!no_node_logged) {
                    log_msg("ERROR", "电量%d 未找到有效充电控制节点！", level);
                    no_node_logged = 1;
                }
            }

            /* power_reset：插电瞬间自动拔插 */
            if (cfg.power_reset && !was_powered && !stopped) {
                char used[MAX_LINE], val[MAX_VAL];
                if (do_power_stop(used, sizeof(used))) {
                    sleep(1);
                    do_power_start(used, sizeof(used), val, sizeof(val));
                    log_msg("INFO", "触发自动拔插");
                }
            }
        }

        /* ── 预约充电判定 ── */
        int sched_state = 0, sched_held = 0;
        time_t sched_deadline = 0, sched_resume = 0, sched_finish = 0;
        int sched_stop = sched_should_stop(level, powered, &cfg,
                                           &sched_state, &sched_deadline,
                                           &sched_resume, &sched_finish);

        /* 未插电时不按住：此时 verify_stopped 会空转通过，无法验证节点真伪 */
        if (sched_stop && !stopped && powered) {
            char used[MAX_LINE];
            if (do_power_stop(used, sizeof(used))) {
                touch_file(POWER_SWITCH_FLAG);
                touch_file(BATTERY_SWITCH_FLAG);
                touch_file(SCHED_HOLD);
                sched_held = 1;
                log_msg("INFO", "预约充电：按住 %d%% [%s]", level, used);
                no_node_logged = 0;
            } else if (!no_node_logged) {
                log_msg("ERROR", "预约充电：未找到有效节点");
                no_node_logged = 1;
            }
        } else if (!sched_stop && stopped && file_exists(SCHED_HOLD)) {
            /* 只处理“由预约按住造成”的停充（v1.3 对所有停充都进这里，击穿了滞回）。
               预约不再按住：普通阈值也不需要停充才恢复；否则交还给普通恢复判定。 */
            int normal_need_stop = 0;
            if (cfg.charge_full) {
                if (level >= 100) normal_need_stop = 1;
            } else if (level >= cfg.power_stop) {
                normal_need_stop = 1;
            }
            int hot = (cfg.temperature_switch && temp >= cfg.temperature_switch_stop);
            if (hot) normal_need_stop = 1;

            log_msg("DEBUG", "预约放行判定: level=%d normal_need_stop=%d", level, normal_need_stop);

            if (!normal_need_stop) {
                char used[MAX_LINE], val[MAX_VAL];
                if (do_power_start(used, sizeof(used), val, sizeof(val))) {
                    rm_file(POWER_SWITCH_FLAG);
                    rm_file(BATTERY_SWITCH_FLAG);
                    rm_file(TEMP_SWITCH_FLAG);
                    rm_file(SCHED_HOLD);
                    sched_held = 0;
                    log_msg("INFO", "预约放行：恢复充电 %d%% [%s]", level, used);
                }
            } else {
                if (hot) touch_file(TEMP_SWITCH_FLAG);
                rm_file(SCHED_HOLD);
            }
        }

        /* 预约块可能改了 flag，刷新本轮视图 */
        stopped = file_exists(POWER_SWITCH_FLAG);
        reason_battery = file_exists(BATTERY_SWITCH_FLAG);
        reason_temp = file_exists(TEMP_SWITCH_FLAG);

        /* 更新 sched_held 状态（用于 status.kv） */
        if (stopped && sched_state == 1) sched_held = 1;

        /* ── 恢复判定 ── */
        if (stopped) {
            int temp_ready = 1, battery_ready = 1;

            if (reason_temp && cfg.temperature_switch && temp > cfg.temperature_switch_start) {
                temp_ready = 0;
            }
            if (reason_battery && level > cfg.power_start) {
                battery_ready = 0;
            }

            /* v1.6：charge_full=1 时充满即停模式完全不受 power_start 约束，
               只要掉到 99% 就立即补电回 100%（浮充），避免插电 99% 一直停充不进电 */
            if (cfg.charge_full && reason_battery && !reason_temp) {
                if (level >= 100) {
                    battery_ready = 0;  /* 100%时保持停充，不恢复 */
                } else {
                    battery_ready = 1;  /* 充满即停：低于100%立即恢复补电，忽略 power_start */
                }
            }

            /* 预约充电：按住状态时不恢复 */
            if (sched_state == 1) {
                battery_ready = 0;
            }

            /* v1.4: 不再在 state=2 强制恢复。放行动作由上面的预约放行分支完成，
               放行后普通阈值(power_stop/power_start)继续生效，目标电量即 power_stop。 */

            log_msg("DEBUG", "恢复判定: stopped=%d sched_state=%d reason_battery=%d level=%d power_start=%d battery_ready=%d",
                    stopped, sched_state, reason_battery, level, cfg.power_start, battery_ready);

            if (temp_ready && battery_ready) {
                sleep(3);
                char used[MAX_LINE], val[MAX_VAL];
                int ret = do_power_start(used, sizeof(used), val, sizeof(val));
                log_msg("DEBUG", "do_power_start ret=%d used=%s val=%s", ret, used, val);
                if (ret) {
                    rm_file(POWER_SWITCH_FLAG);
                    rm_file(BATTERY_SWITCH_FLAG);
                    rm_file(TEMP_SWITCH_FLAG);
                    rm_file(LEAK_COUNT_FILE);
                    if (reason_temp) {
                        log_msg("INFO", "电量%d 触发温控恢复充电 温度%d [%s <- %s]",
                                level, temp, used, val);
                    } else {
                        log_msg("INFO", "电量%d 恢复充电 [%s <- %s]", level, used, val);
                    }
                }
            }
        }

        /* 更新状态 */
        stopped = file_exists(POWER_SWITCH_FLAG);
        verified = 0;
        leak = 0;
        if (stopped) {
            if (strcmp(status, "Not charging") == 0 ||
                strcmp(status, "Discharging") == 0 ||
                strcmp(status, "Full") == 0 ||
                current_says_not_charging(current)) {
                verified = 1;
            } else {
                leak = 1;
            }
        }

        write_status_kv(level, temp, powered, stopped,
                        file_exists(BATTERY_SWITCH_FLAG),
                        file_exists(TEMP_SWITCH_FLAG),
                        file_exists(OFF_FLAG) ? 1 : 0,
                        status, verified, leak,
                        g_pref_valid ? g_pref_path : "",
                        cfg.sched_on, sched_state, (long)sched_deadline,
                        (long)sched_resume, (long)sched_finish, sched_held);

        was_powered = powered;
        /* 循环周期：充电3秒，停充中10秒（等恢复），空闲30秒（省电） */
        if (powered) sleep(3);
        else if (stopped) sleep(10);
        else sleep(30);
    }

    return 0;
}
