/*
 * update_prop.c — Aegis
 * 内容：Aegis 调度状态 | 温控模式 | 充电阈值（含预约）
 *
 * 用法: update_prop <moddir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

static int read_line(const char *path, char *buf, int sz)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, sz, f)) { fclose(f); return -1; }
    fclose(f);
    int len = (int)strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    return 0;
}

static void read_file_all(const char *path, char *buf, int bufsz)
{
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(buf, 1, (size_t)bufsz - 1, f);
    buf[n] = '\0';
    fclose(f);
}

/* 从 "key=value\n..." 文本里取 key；key 必须在行首 */
static void kv_get(const char *content, const char *key, char *out, int outsz)
{
    out[0] = '\0';
    size_t kl = strlen(key);
    const char *p = content;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            const char *nl = strchr(v, '\n');
            int len = nl ? (int)(nl - v) : (int)strlen(v);
            if (len >= outsz) len = outsz - 1;
            memcpy(out, v, (size_t)len);
            out[len] = '\0';
            return;
        }
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
}

/* 从 rules.yaml 取 "key: value" 标量（行首，冒号后空格可选） */
static void yaml_get(const char *content, const char *key, char *out, int outsz)
{
    out[0] = '\0';
    size_t kl = strlen(key);
    const char *p = content;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == ':') {
            const char *v = p + kl + 1;
            while (*v == ' ' || *v == '\t') v++;
            const char *nl = strchr(v, '\n');
            int len = nl ? (int)(nl - v) : (int)strlen(v);
            while (len > 0 && (v[len-1] == '\r' || v[len-1] == ' ' || v[len-1] == '\t')) len--;
            /* 剥除 YAML 引号（"fast" / 'fast' → fast），否则 strcmp 永不匹配 */
            if (len >= 2 && ((v[0] == '"' && v[len-1] == '"') || (v[0] == '\'' && v[len-1] == '\''))) {
                v++; len -= 2;
                while (len > 0 && (v[len-1] == ' ' || v[len-1] == '\t')) len--;
            }
            if (len >= outsz) len = outsz - 1;
            memcpy(out, v, (size_t)len);
            out[len] = '\0';
            return;
        }
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
}

static int file_exists(const char *p) { return access(p, F_OK) == 0; }

static void fmt_hm(const char *epoch, char *out, size_t sz)
{
    time_t t = (time_t)atol(epoch);
    struct tm tmv;
    out[0] = '\0';
    if (localtime_r(&t, &tmv)) strftime(out, sz, "%H:%M", &tmv);
}

static void build_cg_hint(const char *moddir, char *hint, size_t hsz)
{
    char off[600], conf[600], st[600];
    snprintf(off, sizeof(off), "%s/charge_guard/data/off", moddir);
    snprintf(conf, sizeof(conf), "%s/charge_guard.conf", moddir);
    snprintf(st, sizeof(st), "%s/charge_guard/data/status.kv", moddir);

    hint[0] = '\0';
    if (file_exists(off)) { snprintf(hint, hsz, " | 充电阈值:关"); return; }

    char c[2048], s[1024];
    read_file_all(conf, c, sizeof(c));
    read_file_all(st, s, sizeof(s));

    char full[8], stop[8], tsw[8], tstop[8], sched[8], stopped[8];
    kv_get(c, "charge_full", full, sizeof(full));
    kv_get(c, "power_stop", stop, sizeof(stop));
    kv_get(c, "temperature_switch", tsw, sizeof(tsw));
    kv_get(c, "temperature_switch_stop", tstop, sizeof(tstop));
    kv_get(c, "sched_switch", sched, sizeof(sched));
    kv_get(s, "stopped", stopped, sizeof(stopped));

    if (!strcmp(full, "1")) {
        snprintf(hint, hsz, " | 充电阈值:充满即停%s", !strcmp(stopped, "1") ? "(已停充)" : "");
    } else if (stop[0] && strcmp(stop, "110") != 0) {
        if (!strcmp(tsw, "1") && tstop[0])
            snprintf(hint, hsz, " | 充电阈值:限充%s%% · %s℃停充", stop, tstop);
        else
            snprintf(hint, hsz, " | 充电阈值:限充%s%%", stop);
    } else if (!strcmp(tsw, "1") && tstop[0]) {
        snprintf(hint, hsz, " | 充电阈值:仅%s℃停充", tstop);
    } else {
        snprintf(hint, hsz, " | 充电阈值:关");
    }

    if (!strcmp(sched, "1")) {
        char on[8], state[8], dl[32], fin[32], tb[16];
        kv_get(s, "sched_on", on, sizeof(on));
        kv_get(s, "sched_state", state, sizeof(state));
        kv_get(s, "sched_deadline", dl, sizeof(dl));
        kv_get(s, "sched_finish", fin, sizeof(fin));
        if (!strcmp(on, "1")) {
            size_t len = strlen(hint);
            int st_i = atoi(state);
            if (st_i == 1)      { fmt_hm(dl,  tb, sizeof(tb)); snprintf(hint + len, hsz - len, " | 预约:按住→%s", tb); }
            else if (st_i == 2) { fmt_hm(fin, tb, sizeof(tb)); snprintf(hint + len, hsz - len, " | 预约:充电→%s", tb); }
            else if (st_i == 3) snprintf(hint + len, hsz - len, " | 预约:低电先充");
            else if (st_i == 4) snprintf(hint + len, hsz - len, " | 预约:待命");
            else if (st_i == 5) snprintf(hint + len, hsz - len, " | 预约:已充满");
            else if (st_i == 6) snprintf(hint + len, hsz - len, " | 预约:已跳过");
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2) { fprintf(stderr, "usage: update_prop <moddir>\n"); return 1; }
    const char *moddir = argv[1];

    /* 调度状态从 rules.yaml 读取  */
    char rules[4096];
    char rp[600];
    snprintf(rp, sizeof(rp), "%s/rules.yaml", moddir);
    read_file_all(rp, rules, sizeof(rules));

    char enabled[16] = "true", mode[32] = "balance", therm[32] = "official";
    yaml_get(rules, "aegis_scheduler", enabled, sizeof(enabled));
    if (enabled[0] == '\0') yaml_get(rules, "yumi_scheduler", enabled, sizeof(enabled));
    yaml_get(rules, "global_mode", mode, sizeof(mode));

    char tp[600];
    snprintf(tp, sizeof(tp), "%s/thermal_mode", moddir);
    read_line(tp, therm, sizeof(therm));

    const char *sched;
    if (!strcmp(enabled, "false"))          sched = "⏸ Aegis：已关闭";
    else if (!strcmp(mode, "powersave"))    sched = "🌙 Aegis：省电";
    else if (!strcmp(mode, "performance"))  sched = "🚀 Aegis：性能";
    else if (!strcmp(mode, "fast"))         sched = "🔥 Aegis：极速";
    else if (!strcmp(mode, "fas"))          sched = "🎮 Aegis：帧感";
    else                                    sched = "⚖️ Aegis：标准";

    const char *wk;
    if (!strcmp(therm, "jizhi"))        wk = "🔥 极致性能";
    else if (!strcmp(therm, "sangxin")) wk = "💀 丧心病狂";
    else                                wk = "🌡️ 官方温控";

    char cg[320];
    build_cg_hint(moddir, cg, sizeof(cg));

    char desc[840];
    snprintf(desc, sizeof(desc), "description=%s | %s%s", sched, wk, cg);

    /* 读原 module.prop，除 description 外原样保留，写临时文件后 rename */
    char prop[600], tmp[620];
    snprintf(prop, sizeof(prop), "%s/module.prop", moddir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", prop);

    FILE *in = fopen(prop, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) { perror("update_prop: open tmp"); if (in) fclose(in); return 1; }

    int wrote = 0;
    if (in) {
        char line[1024];
        while (fgets(line, sizeof(line), in)) {
            if (!strncmp(line, "description=", 12)) {
                if (!wrote) { fprintf(out, "%s\n", desc); wrote = 1; }
            } else {
                fputs(line, out);
            }
        }
        fclose(in);
    } else {
        fprintf(out,
            "id=aegis_power_matrix\n"
            "name=Aegis\n"
            "version=2\n"
            "versionCode=2\n"
            "author=默默的阿文\n");
    }
    if (!wrote) fprintf(out, "%s\n", desc);
    fclose(out);
    if (rename(tmp, prop) != 0) { perror("update_prop: rename"); return 1; }
    return 0;
}