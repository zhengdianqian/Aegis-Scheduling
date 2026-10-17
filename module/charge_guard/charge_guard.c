/*
 * charge_guard.c — Aegis 充电阈值守护 v1.3
 * 移植自止观 shell 版本，加入真断电验证、假节点拉黑、preferred 重验、停充期自愈
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

#define MODDIR "/data/adb/modules/aegis_power_matrix"
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

static void log_msg(const char *level, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* 只对非 DEBUG 级别去重 */
    if (strcmp(level, "DEBUG") != 0) {
        time_t now = time(NULL);
        if (strcmp(msg, g_last_log) == 0 && (now - g_last_log_time) < 60) return;
        strncpy(g_last_log, msg, sizeof(g_last_log)-1);
        g_last_log_time = now;
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
        while (fgets(lines[n], MAX_LINE, fp) && n < MAX_NODES) {
            char p[MAX_PATH]; long ts;
            if (sscanf(lines[n], "%255s %ld", p, &ts) == 2) {
                if ((now - ts) < 604800) n++;
            }
        }
        fclose(fp);
    }
    /* 去重追加 */
    if (!is_bad_switch(path)) {
        snprintf(lines[n], MAX_LINE, "%s %ld\n", path, (long)now);
        n++;
    }
    fp = fopen(BAD_SWITCH, "w");
    if (!fp) return;
    for (int i = 0; i < n; i++) fputs(lines[i], fp);
    fclose(fp);
}

/* ── 限流节点原值保存/恢复 ── */
static void save_orig_value(const char *path, const char *val) {
    FILE *fp = fopen(ORIG_VALUES_FILE, "r");
    char lines[MAX_NODES][MAX_LINE];
    int n = 0, found = 0;
    if (fp) {
        while (fgets(lines[n], MAX_LINE, fp) && n < MAX_NODES) {
            char p[MAX_PATH];
            if (sscanf(lines[n], "%255s", p) == 1 && strcmp(p, path) == 0) {
                snprintf(lines[n], MAX_LINE, "%s %s\n", path, val);
                found = 1;
            }
            n++;
        }
        fclose(fp);
    }
    if (!found) {
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

/* ── 真断电验证：轮询 status + current_now ── */
static int verify_stopped(int timeout_sec) {
    char status[64];
    for (int i = 0; i < timeout_sec; i++) {
        if (read_file("/sys/class/power_supply/battery/status", status, sizeof(status)) == 0) {
            if (strcmp(status, "Not charging") == 0 ||
                strcmp(status, "Discharging") == 0 ||
                strcmp(status, "Full") == 0) {
                return 1;
            }
        }
        /* 部分机型 status 不变但电流转正（放电） */
        int cur = read_int_file("/sys/class/power_supply/battery/current_now", 0);
        if (cur > 0) return 1;
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
            log_msg("DEBUG", "config: %s=%s", key, val);
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

    if (read_file("/sys/class/power_supply/battery/capacity", buf, sizeof(buf)) < 0)
        return -1;
    *level = atoi(buf);

    if (read_file("/sys/class/power_supply/battery/temp", buf, sizeof(buf)) == 0) {
        int t = atoi(buf);
        if (t > 200) *temp = t / 10; /* 部分机型是 10 倍 */
        else *temp = t;
    } else {
        *temp = 25;
    }

    if (read_file("/sys/class/power_supply/battery/status", status, st_size) < 0)
        strcpy(status, "Unknown");

    *current = read_int_file("/sys/class/power_supply/battery/current_now", 0);

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
            if (sscanf(line, "%255[^,],start=%63[^,],stop=%63s", path, sv, sp) == 3) {
                add_node(path, sv, sp);
            }
        }
        fclose(fp);
    }

    /* 内置兜底列表（与 shell 版一致） */
    static const struct { const char *p, *s, *t; } fallback[] = {
        {"/sys/class/power_supply/battery/charge_charger_state","1","0"},
        {"/sys/class/power_supply/battery/connect_disable","0","1"},
        {"/sys/class/power_supply/battery/batt_slate_mode","0","1"},
        {"/sys/class/power_supply/battery/store_mode","0","1"},
        {"/sys/class/power_supply/battery/input_suspend","0","1"},
        {"/sys/class/power_supply/battery/battery_input_suspend","0","1"},
        {"/sys/class/power_supply/battery/night_charging","0","1"},
        {"/sys/class/power_supply/battery/charging_enabled","1","0"},
        {"/sys/class/power_supply/battery/battery_charging_enabled","1","0"},
        {"/sys/class/power_supply/battery/batt_charging_enabled","1","0"},
        {"/sys/class/power_supply/battery/charge_disable","0","1"},
        {"/sys/class/power_supply/battery/disable_charging","0","1"},
        {"/sys/class/power_supply/battery/stop_charging","0","1"},
        {"/sys/class/power_supply/battery/charge_enabled","1","0"},
        {"/sys/class/power_supply/battery/op_disable_charge","0","1"},
        {"/sys/class/power_supply/battery/device/Charging_Enable","1","0"},
        {"/sys/class/power_supply/battery/ChargerEnable","1","0"},
        {"/sys/class/power_supply/battery/force_disable_charging","0","1"},
        {"/sys/class/power_supply/battery/charge_control_enabled","1","0"},
        {"/sys/class/power_supply/battery/mi_charge_enable","1","0"},
        {"/sys/class/power_supply/battery_ext/smart_charging_interruption","0","1"},
        {"/sys/class/power_supply/main/adapter_cc_mode","0","1"},
        {"/sys/class/power_supply/main/cool_mode","0","1"},
        {"/sys/class/power_supply/charger/charge_disable","0","1"},
        {"/sys/class/power_supply/bms/charge_disable","0","1"},
        {"/sys/class/power_supply/bms/charging_enabled","1","0"},
        {"/sys/class/power_supply/bms/charge_enabled","1","0"},
        {"/sys/class/power_supply/mi_chg/charge_disable","0","1"},
        {"/sys/class/power_supply/mi_chg/charging_enabled","1","0"},
        {"/sys/class/qcom-battery/charging_enabled","1","0"},
        {"/sys/class/qcom-battery/charge_disable","0","1"},
        {"/sys/class/qcom-battery/input_suspend","0","1"},
        {"/sys/class/qcom-battery/battery_charging_enabled","1","0"},
        {"/sys/class/qcom-battery/charging_suspend_battery","0","1"},
        {"/sys/class/qcom-battery/night_charging","0","1"},
        {"/sys/class/qcom-battery/cool_mode","0","1"},
        {"/sys/class/qcom-battery/batt_protect_en","0","1"},
        {"/sys/class/asuslib/charger_limit_en","0","1"},
        {"/sys/class/asuslib/charging_suspend_en","0","1"},
        {"/sys/class/hw_power/charger/charge_data/enable_charger","1","0"},
        {"/sys/class/power_supply/idt/pin_enabled","1","0"},
        {"/sys/devices/platform/huawei_charger/enable_charger","1","0"},
        {"/sys/devices/platform/lge-unified-nodes/charging_enable","1","0"},
        {"/sys/devices/platform/lge-unified-nodes/charging_completed","0","1"},
        {"/sys/devices/platform/mt-battery/disable_charger","0","1"},
        {"/sys/devices/platform/battery/ChargerEnable","1","0"},
        {"/sys/devices/platform/soc/soc:google,charger/charge_disable","0","1"},
        {"/sys/devices/platform/charger/bypass_charger","0","1"},
        {"/sys/devices/platform/soc/soc:qcom,pmic_glink/soc:qcom,pmic_glink:qcom,battery_charger/force_charger_suspend","0","1"},
        {"/sys/devices/virtual/oplus_chg/battery/mmi_charging_enable","1","0"},
        {"/sys/kernel/debug/google_charger/chg_suspend","0","1"},
        {"/sys/kernel/debug/google_charger/input_suspend","0","1"},
        {"/sys/kernel/debug/google_charger/chg_mode","1","0"},
        {"/sys/kernel/nubia_charge/charger_bypass","off","on"},
        {"/sys/module/qpnp_adaptive_charge/parameters/blocking","0","1"},
        {"/proc/driver/charger_limit_enable","0","1"},
        {"/proc/driver/charger_limit","100","1"},
        {"/proc/mtk_battery_cmd/current_cmd","0 0","0 1"},
        {"/proc/mtk_battery_cmd/en_power_path","1","0"},
        {"/sys/class/power_supply/battery/constant_charge_current_max","3000000","0"},
        {"/sys/class/power_supply/battery/current_max","3000000","0"},
        {"/sys/class/power_supply/battery/input_current_max","3000000","0"},
        {"/sys/class/power_supply/battery/charge_current","3000000","0"},
        {"/sys/class/power_supply/battery/fast_charge_current_max","3000000","0"},
        {"/sys/class/power_supply/usb/input_suspend","0","1"},
        {"/sys/class/power_supply/qc_usb/input_suspend","0","1"},
        {"/sys/class/power_supply/dc/input_suspend","0","1"},
        {"/sys/class/power_supply/ac/device/power_supply/usb/power_switch","1","0"},
        {"/sys/class/power_supply/ac/device/power_supply/usb/input_suspend","0","1"},
        {"/sys/class/power_supply/battery/charge_control_end_threshold","100","0"},
        {"/sys/class/power_supply/battery/charge_type","Fast","None"},
        {"/sys/class/power_supply/pc_port/input_suspend","0","1"},
        {"/sys/class/power_supply/wireless/input_suspend","0","1"},
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
        while (fgets(lines[n], MAX_LINE, fp) && n < 64) n++;
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

/* ── 停充：遍历节点，验证真断电 ── */
static int do_power_stop(char *used_node, size_t node_size) {
    /* 优先 preferred */
    if (g_pref_valid && !is_bad_switch(g_pref_path) && file_exists(g_pref_path)) {
        if (write_node(g_pref_path, g_pref_stop) == 0) {
            if (verify_stopped(5)) {
                snprintf(used_node, node_size, "%s=%s (preferred)", g_pref_path, g_pref_stop);
                return 1;
            }
            /* preferred 失效：还原 + 清除 + 拉黑 */
            write_node(g_pref_path, g_pref_start);
            clear_preferred();
            blacklist_switch(g_pref_path);
            log_msg("WARN", "preferred 节点验证失败已清除 %s", g_pref_path);
        }
    }

    /* 限流节点排最后 */
    SwitchNode *normal[MAX_NODES], *limit[MAX_NODES];
    int n_normal = 0, n_limit = 0;
    for (int i = 0; i < g_node_count; i++) {
        if (is_bad_switch(g_nodes[i].path)) continue;
        if (!file_exists(g_nodes[i].path)) continue;
        if (g_nodes[i].is_current_limit) limit[n_limit++] = &g_nodes[i];
        else normal[n_normal++] = &g_nodes[i];
    }

    /* 先尝试非限流节点 */
    for (int i = 0; i < n_normal; i++) {
        SwitchNode *n = normal[i];
        if (write_node(n->path, n->stop_val) != 0) continue;
        if (verify_stopped(5)) {
            snprintf(used_node, node_size, "%s=%s", n->path, n->stop_val);
            save_preferred(n->path, n->start_val, n->stop_val);
            load_preferred();
            return 1;
        }
        /* 假节点：还原 + 拉黑 */
        write_node(n->path, n->start_val);
        blacklist_switch(n->path);
        log_msg("WARN", "假节点（写入成功但未断电）已拉黑 %s", n->path);
    }

    /* 最后尝试限流节点 */
    for (int i = 0; i < n_limit; i++) {
        SwitchNode *n = limit[i];
        char orig[MAX_VAL];
        if (read_file(n->path, orig, sizeof(orig)) == 0) {
            save_orig_value(n->path, orig);
        }
        if (write_node(n->path, n->stop_val) != 0) continue;
        if (verify_stopped(5)) {
            snprintf(used_node, node_size, "%s=%s", n->path, n->stop_val);
            save_preferred(n->path, n->start_val, n->stop_val);
            load_preferred();
            return 1;
        }
        /* 恢复原值 + 拉黑 */
        if (get_orig_value(n->path, orig, sizeof(orig)) == 0) {
            write_file_raw(n->path, orig);
        } else {
            write_node(n->path, n->start_val);
        }
        blacklist_switch(n->path);
        log_msg("WARN", "假节点（限流型）已拉黑 %s", n->path);
    }

    return 0;
}

/* ── 恢复充电 ── */
static int do_power_start(char *used_node, size_t node_size, char *used_val, size_t val_size) {
    /* 优先 preferred */
    if (g_pref_valid && file_exists(g_pref_path)) {
        if (write_node(g_pref_path, g_pref_start) == 0) {
            snprintf(used_node, node_size, "%s", g_pref_path);
            snprintf(used_val, val_size, "%s", g_pref_start);
            return 1;
        }
    }

    /* 遍历所有节点写 start 值 */
    for (int i = 0; i < g_node_count; i++) {
        SwitchNode *n = &g_nodes[i];
        if (!file_exists(n->path)) continue;

        const char *val = n->start_val;
        char orig[MAX_VAL];
        if (n->is_current_limit) {
            if (get_orig_value(n->path, orig, sizeof(orig)) == 0) {
                val = orig;
            }
        }

        if (write_node(n->path, val) == 0) {
            snprintf(used_node, node_size, "%s", n->path);
            snprintf(used_val, val_size, "%s", val);
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

/* 预约充电判定：返回是否需要停充 */
static int sched_should_stop(int level, int powered, const Config *cfg,
                             int *state, time_t *deadline, time_t *resume, time_t *finish) {
    *state = 0;
    *deadline = 0;
    *resume = 0;
    *finish = 0;

    if (!cfg->sched_on) return 0;
    if (file_exists(SCHED_SKIP)) return 0; /* 本次已跳过 */

    time_t now = time(NULL);
    time_t target_time = parse_sched_time(cfg->sched_time);
    int charge_min = estimate_charge_minutes(level, 100);
    time_t start_time = target_time - (cfg->sched_margin * 60) - (charge_min * 60);
    time_t window_start = target_time - (cfg->sched_window * 3600);

    *deadline = target_time;
    *resume = start_time;
    *finish = target_time;

    /* 窗口外：不干预 */
    if (now < window_start || now > target_time + 3600) {
        *state = 0;
        return 0;
    }

    /* 已到目标时间且电量满：停充 */
    if (now >= target_time && level >= 100) {
        *state = 5;
        return 1;
    }

    /* 保底电量：直接充 */
    if (level < cfg->sched_floor) {
        *state = 3;
        return 0;
    }

    /* 窗口内：
       - 还没到开始充电时间：按住（停充）
       - 到了开始充电时间：放行（充电） */
    if (now < start_time) {
        *state = 1; /* 已按住 */
        return 1;
    }

    *state = 2; /* 已放行 */
    return 0;
}

/* ── 写 status.kv ── */
static void write_status_kv(int level, int temp, int powered, int stopped,
                            int reason_battery, int reason_temp, int off,
                            const char *bat_status, int verified, int leak,
                            const char *node,
                            int sched_on, int sched_state, long sched_deadline,
                            long sched_resume, long sched_finish, int sched_held) {
    FILE *fp = fopen(STATUS_KV, "w");
    if (!fp) return;
    time_t now = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H:%M:%S", localtime(&now));

    fprintf(fp, "level=%d\n", level);
    fprintf(fp, "temp=%d\n", temp);
    fprintf(fp, "powered=%d\n", powered);
    fprintf(fp, "stopped=%d\n", stopped);
    fprintf(fp, "reason_battery=%d\n", reason_battery);
    fprintf(fp, "reason_temp=%d\n", reason_temp);
    fprintf(fp, "off=%d\n", off);
    fprintf(fp, "bat_status=%s\n", bat_status);
    fprintf(fp, "verified=%d\n", verified);
    fprintf(fp, "leak=%d\n", leak);
    fprintf(fp, "node=%s\n", node);
    fprintf(fp, "sched_on=%d\n", sched_on);
    fprintf(fp, "sched_state=%d\n", sched_state);
    fprintf(fp, "sched_deadline=%ld\n", sched_deadline);
    fprintf(fp, "sched_resume=%ld\n", sched_resume);
    fprintf(fp, "sched_finish=%ld\n", sched_finish);
    fprintf(fp, "sched_held=%d\n", sched_held);
    fprintf(fp, "updated_at=%s\n", ts);
    fclose(fp);
}

/* ── 主逻辑 ── */
int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        printf("charge_guardd v1.3 (Aegis)\n");
        return 0;
    }
    (void)argc; (void)argv;

    mkdir(DATADIR, 0755);

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

    log_msg("INFO", "charge_guard v1.3 启动");

    /* 启动时状态一致性检查：如果节点是停充状态但 flag 不存在，恢复节点 */
    if (!file_exists(POWER_SWITCH_FLAG)) {
        scan_nodes();
        load_preferred();
        for (int i = 0; i < g_node_count; i++) {
            char cur[MAX_VAL];
            if (read_file(g_nodes[i].path, cur, sizeof(cur)) == 0) {
                if (strcmp(cur, g_nodes[i].stop_val) == 0) {
                    const char *restore_val = g_nodes[i].start_val;
                    char orig[MAX_VAL];
                    if (g_nodes[i].is_current_limit &&
                        get_orig_value(g_nodes[i].path, orig, sizeof(orig)) == 0) {
                        restore_val = orig;
                    }
                    write_node(g_nodes[i].path, restore_val);
                    log_msg("INFO", "启动恢复节点 %s <- %s", g_nodes[i].path, restore_val);
                }
            }
        }
    }

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
                current > 0) {
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
                if (level >= 100) {
                    static int low_current_count = 0;
                    if (strcmp(status, "Full") == 0) {
                        rm_file(NOW_C_FILE);
                        low_current_count = 0;
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
                            log_msg("DEBUG", "电量%d 充满再停：涓流已结束", level);
                        }
                    }
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

        /* ── 预约充电判定（优先级高于普通阈值） ── */
        int sched_state = 0, sched_held = 0;
        time_t sched_deadline = 0, sched_resume = 0, sched_finish = 0;
        log_msg("INFO", "预约前: sched_on=%d stopped=%d powered=%d", cfg.sched_on, stopped, powered);
        int sched_stop = sched_should_stop(level, powered, &cfg,
                                           &sched_state, &sched_deadline,
                                           &sched_resume, &sched_finish);
        log_msg("INFO", "预约后: sched_stop=%d sched_state=%d", sched_stop, sched_state);

        if (sched_stop && !stopped) {
            /* 预约需要停充（标记为电量原因，防止恢复判定误恢复） */
            char used[MAX_LINE];
            if (do_power_stop(used, sizeof(used))) {
                touch_file(POWER_SWITCH_FLAG);
                touch_file(BATTERY_SWITCH_FLAG); /* v1.3: 预约停充也标记电量原因 */
                sched_held = 1;
                log_msg("INFO", "预约充电：按住 %d%% [%s]", level, used);
                no_node_logged = 0;
            } else if (!no_node_logged) {
                log_msg("ERROR", "预约充电：未找到有效节点");
                no_node_logged = 1;
            }
        } else if (!sched_stop && stopped) {
            /* 预约已关闭或放行，检查是否应恢复 */
            int normal_need_stop = 0;
            if (powered) {
                if (!cfg.charge_full && level >= cfg.power_stop) normal_need_stop = 1;
                if (cfg.charge_full && level >= 100) normal_need_stop = 1;
                if (cfg.temperature_switch && temp >= cfg.temperature_switch_stop) normal_need_stop = 1;
            }

            log_msg("INFO", "预约关闭判定: powered=%d normal_need_stop=%d level=%d power_stop=%d charge_full=%d",
                    powered, normal_need_stop, level, cfg.power_stop, cfg.charge_full);

            if (!normal_need_stop) {
                /* 普通逻辑不需要停充，恢复 */
                char used[MAX_LINE], val[MAX_VAL];
                if (do_power_start(used, sizeof(used), val, sizeof(val))) {
                    rm_file(POWER_SWITCH_FLAG);
                    rm_file(BATTERY_SWITCH_FLAG);
                    rm_file(TEMP_SWITCH_FLAG);
                    sched_held = 0;
                    log_msg("INFO", "预约关闭：恢复充电 %d%% [%s]", level, used);
                }
            }
        }

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

            /* charge_full=1 时：如果电量<100且是电量原因停充，恢复充电直到100 */
            if (cfg.charge_full && reason_battery && level < 100 && !stop_by_temp) {
                battery_ready = 1;
            }

            /* 预约充电：按住状态时不恢复 */
            if (sched_state == 1) {
                battery_ready = 0;
            }

            log_msg("INFO", "恢复判定: stopped=%d sched_state=%d reason_battery=%d level=%d power_start=%d battery_ready=%d",
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
                current > 0) {
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
