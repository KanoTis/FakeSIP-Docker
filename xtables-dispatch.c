/*
 * xtables-dispatch.c - FakeSIP-Docker
 *
 * Installed as /usr/sbin/iptables, /usr/sbin/ip6tables and /usr/sbin/nft.
 *
 * 1. Loads the kernel modules FakeSIP needs, best effort, when the host module
 *    tree is bind mounted (/lib/modules). Older kernels such as Synology DSM
 *    do not autoload xt_connbytes / xt_NFQUEUE.
 *
 * 2. iptables / ip6tables: prefers the nft backed xtables binary and falls
 *    back to the legacy one when nf_tables is unavailable or broken. The
 *    choice is cached per command name in /run.
 *
 * 3. When the kernel provides no connbytes match, the "-m connbytes" rule is
 *    rewritten: first as a per-flow hashlimit that emulates "first N packets",
 *    and if even that is missing, as "-m conntrack --ctstate NEW".
 *
 * 4. nft: loads the modules and hands over to the real static nft.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define NFT_MULTI "/usr/sbin/xtables-nft-multi"
#define LEGACY_MULTI "/usr/sbin/xtables-legacy-multi"
#define REAL_NFT "/usr/sbin/nft.real"

#define STATE_DIR "/run"
#define PROBE_CHAIN "FAKESIP_CBPROBE"
#define CHAIN_PREFIX "FAKESIP_"
#define CHAIN_S "FAKESIP_S"
#define CHAIN_D "FAKESIP_D"
#define CHAIN_R "FAKESIP_R"

enum { BACKEND_UNKNOWN = 0, BACKEND_NFT = 1, BACKEND_LEGACY = 2 };
enum { MATCH_CONNBYTES = 0, MATCH_HASHLIMIT = 1, MATCH_CONNTRACK = 2,
       MATCH_ALL = 3 };

static const char *const modules_iptables[] = {
    "xt_connbytes", "xt_NFQUEUE",   "nfnetlink_queue", "xt_hashlimit",
    "xt_conntrack", "xt_mark",      "nf_conntrack",    "nf_defrag_ipv4",
    "nf_defrag_ipv6", NULL
};

static const char *const modules_nft[] = {
    "nf_tables", "nft_queue",  "nft_ct", "nfnetlink_queue",
    "xt_NFQUEUE", "xt_connbytes", NULL
};

/* ------------------------------------------------------------------ utils */

static const char *base_name(const char *path)
{
    const char *slash;

    if (!path || !*path) {
        return "iptables";
    }
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int run_multi(const char *exe, char **argv, const char *argv0)
{
    int status;
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        return 1;
    }
    if (!pid) {
        argv[0] = (char *) argv0;
        execv(exe, argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

/* --------------------------------------------------------- module loading */

static int module_loaded(const char *name)
{
    char line[512], mod[128], *p;
    FILE *fp;
    size_t i;

    fp = fopen("/proc/modules", "r");
    if (!fp) {
        return 1; /* cannot tell, assume loaded */
    }
    while (fgets(line, sizeof(line), fp)) {
        p = strchr(line, ' ');
        if (!p) {
            continue;
        }
        if ((size_t) (p - line) >= sizeof(mod)) {
            continue;
        }
        memcpy(mod, line, (size_t) (p - line));
        mod[p - line] = 0;
        for (i = 0; mod[i]; i++) {
            if (mod[i] == '-') {
                mod[i] = '_';
            }
        }
        if (!strcmp(mod, name)) {
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

static int load_module_file(const char *path)
{
#ifdef SYS_finit_module
    int fd, res;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    res = (int) syscall(SYS_finit_module, fd, "", 0);
    close(fd);
    return res;
#else
    (void) path;
    return -1;
#endif
}

static int scan_dir(const char *dir, const char *want, int depth)
{
    DIR *d;
    struct dirent *e;
    char path[512];
    size_t len, wlen;

    if (depth > 8) {
        return -1;
    }
    d = opendir(dir);
    if (!d) {
        return -1;
    }
    wlen = strlen(want);
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        len = (size_t) snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (len >= sizeof(path)) {
            continue;
        }
        if (e->d_type == DT_DIR) {
            if (!scan_dir(path, want, depth + 1)) {
                closedir(d);
                return 0;
            }
            continue;
        }
        if (strncmp(e->d_name, want, wlen) || strncmp(e->d_name + wlen, ".ko", 3)) {
            continue;
        }
        if (!load_module_file(path)) {
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

static void load_modules(const char *const *mods)
{
    struct utsname u;
    char dir[256];
    int i;

    if (!mods || !*mods) {
        return;
    }
    if (getenv("FAKESIP_NO_MODPROBE")) {
        return;
    }
    if (uname(&u)) {
        return;
    }
    if (snprintf(dir, sizeof(dir), "/lib/modules/%s", u.release) < 0) {
        return;
    }
    if (access(dir, R_OK)) {
        return; /* module tree not mounted, nothing we can do */
    }
    for (i = 0; mods[i]; i++) {
        if (module_loaded(mods[i])) {
            continue;
        }
        scan_dir(dir, mods[i], 0);
    }
}

/* -------------------------------------------------------- state helpers */

/*
 * State files are named "/run/xtables.<name>", where <name> is the command
 * name ("iptables" or "ip6tables"), so both address families are tracked
 * independently. Match probes use "connbytes" / "hashlimit".
 */

static void state_path(char *out, size_t outlen, const char *name)
{
    snprintf(out, outlen, STATE_DIR "/xtables.%s", name);
}

static int read_state(const char *file, const char *expect)
{
    char path[128], buf[32];
    FILE *fp;

    state_path(path, sizeof(path), file);
    fp = fopen(path, "r");
    if (!fp) {
        return 0;
    }
    if (!fgets(buf, sizeof(buf), fp)) {
        buf[0] = 0;
    }
    fclose(fp);
    return !strncmp(buf, expect, strlen(expect));
}

static void write_state(const char *file, const char *value)
{
    char path[128];
    FILE *fp;

    state_path(path, sizeof(path), file);
    fp = fopen(path, "w");
    if (!fp) {
        return;
    }
    fputs(value, fp);
    fputc(10, fp);
    fclose(fp);
}

/* ---------------------------------------------------------- match probes */

static int probe_match(const char *exe, const char *argv0, int kind)
{
    char *new_cmd[] = {"iptables", "-w", "-t", "mangle", "-N",
                       (char *) PROBE_CHAIN, NULL};
    char *del_cmd[] = {"iptables", "-w", "-t", "mangle", "-X",
                       (char *) PROBE_CHAIN, NULL};
    char *bytes_cmd[] = {"iptables", "-w", "-t", "mangle", "-A",
                         (char *) PROBE_CHAIN, "-p", "udp", "-m", "connbytes",
                         "--connbytes", "1:1", "--connbytes-mode", "packets",
                         "--connbytes-dir", "both", "-j", "RETURN", NULL};
    char *hash_cmd[] = {"iptables", "-w", "-t", "mangle", "-A",
                        (char *) PROBE_CHAIN, "-p", "udp", "-m", "hashlimit",
                        "--hashlimit-upto", "10/hour", "--hashlimit-burst", "5",
                        "--hashlimit-mode", "srcip,dstip,dstport",
                        "--hashlimit-name", "fakesipprobe",
                        "--hashlimit-htable-expire", "60000", "-j", "RETURN",
                        NULL};
    const char *state = kind == MATCH_CONNBYTES ? "connbytes" : "hashlimit";
    char **cmd = kind == MATCH_CONNBYTES ? bytes_cmd : hash_cmd;
    int res;

    if (getenv("FAKESIP_NO_CONNBYTES")) {
        return 0;
    }
    if (read_state(state, "ok")) {
        return 1;
    }
    if (read_state(state, "no")) {
        return 0;
    }

    run_multi(exe, new_cmd, argv0);
    res = run_multi(exe, cmd, argv0);
    run_multi(exe, del_cmd, argv0);

    if (!res) {
        write_state(state, "ok");
        return 1;
    }
    write_state(state, "no");
    return 0;
}

/*
 * Rewrite "-m connbytes --connbytes A:B --connbytes-dir D --connbytes-mode M"
 * into a per-flow hashlimit approximation of "first B packets" or, as the last
 * resort, into "-m conntrack --ctstate NEW" (first packet only).
 */
static char **rewrite_connbytes(char **argv, int kind)
{
    static char burst[16];
    char **out;
    int i, j = 0;

    burst[0] = 0;
    for (i = 0; argv[i]; i++) {
        if (!strcmp(argv[i], "--connbytes") && argv[i + 1]) {
            const char *colon = strchr(argv[i + 1], ':');
            const char *val = colon ? colon + 1 : argv[i + 1];
            long n = strtol(val, NULL, 10);

            if (n < 1) {
                n = 1;
            }
            if (n > 32) {
                n = 32;
            }
            snprintf(burst, sizeof(burst), "%ld", n);
            break;
        }
    }
    if (!burst[0]) {
        snprintf(burst, sizeof(burst), "5");
    }

    out = calloc(96, sizeof(*out));
    if (!out) {
        return argv;
    }
    for (i = 0; argv[i] && j < 80; i++) {
        if (!strcmp(argv[i], "-m") && argv[i + 1] &&
            !strcmp(argv[i + 1], "connbytes")) {
            if (kind == MATCH_ALL) {
                /* queue every matching packet */
                i++;
                continue;
            }
            out[j++] = "-m";
            if (kind == MATCH_HASHLIMIT) {
                out[j++] = "hashlimit";
                out[j++] = "--hashlimit-upto";
                out[j++] = "10/hour";
                out[j++] = "--hashlimit-burst";
                out[j++] = burst;
                out[j++] = "--hashlimit-mode";
                out[j++] = "srcip,dstip,dstport";
                out[j++] = "--hashlimit-name";
                out[j++] = "fakesip";
                out[j++] = "--hashlimit-htable-expire";
                out[j++] = "60000";
            } else {
                out[j++] = "conntrack";
            }
            i++;
            continue;
        }
        if (!strcmp(argv[i], "--connbytes") && argv[i + 1]) {
            if (kind == MATCH_HASHLIMIT || kind == MATCH_ALL) {
                /* hashlimit options are emitted with the match;
                   MATCH_ALL queues everything */
            } else {
                out[j++] = "--ctstate";
                out[j++] = "NEW";
            }
            i++;
            continue;
        }
        if (!strcmp(argv[i], "--connbytes-dir") ||
            !strcmp(argv[i], "--connbytes-mode")) {
            i++;
            continue;
        }
        out[j++] = argv[i];
    }

    out[j] = NULL;
    return out;
}

static int has_connbytes(char **argv)
{
    int i;

    for (i = 0; argv[i]; i++) {
        if (!strcmp(argv[i], "connbytes")) {
            return 1;
        }
    }
    return 0;
}

static int retry_with(char **argv, int kind, const char *exe,
                      const char *argv0)
{
    char **fixed = rewrite_connbytes(argv, kind);
    int res = run_multi(exe, fixed, argv0);

    if (fixed != argv) {
        free(fixed);
    }
    return res;
}

/* ------------------------------------------------- stale rule recovery */

/*
 * A previous FakeSIP run that died before its cleanup (or a container that was
 * killed) can leave FAKESIP_S / FAKESIP_D / FAKESIP_R behind. Every aborted
 * run adds one more "-I PREROUTING -j FAKESIP_S" jump, so the following run
 * fails with "Chain already exists" and never gets going. Detect the chain
 * creation command and purge the leftovers once before retrying.
 */

static int is_chain_create(char **argv)
{
    int i;

    for (i = 1; argv[i]; i++) {
        if (strcmp(argv[i], "-N")) {
            continue;
        }
        if (argv[i + 1] && !strncmp(argv[i + 1], CHAIN_PREFIX, 7)) {
            return 1;
        }
    }
    return 0;
}

static void purge_stale(const char *exe, const char *argv0)
{
    char *del_s[] = {"iptables", "-w", "-t", "mangle", "-D", "PREROUTING",
                     "-j", (char *) CHAIN_S, NULL};
    char *del_d[] = {"iptables", "-w", "-t", "mangle", "-D", "POSTROUTING",
                     "-j", (char *) CHAIN_D, NULL};
    char *flush_r[] = {"iptables", "-w", "-t", "mangle", "-F",
                       (char *) CHAIN_R, NULL};
    char *flush_s[] = {"iptables", "-w", "-t", "mangle", "-F",
                       (char *) CHAIN_S, NULL};
    char *flush_d[] = {"iptables", "-w", "-t", "mangle", "-F",
                       (char *) CHAIN_D, NULL};
    char *del_r[] = {"iptables", "-w", "-t", "mangle", "-X",
                     (char *) CHAIN_R, NULL};
    char *del_cs[] = {"iptables", "-w", "-t", "mangle", "-X",
                      (char *) CHAIN_S, NULL};
    char *del_cd[] = {"iptables", "-w", "-t", "mangle", "-X",
                      (char *) CHAIN_D, NULL};
    int i;

    for (i = 0; i < 16; i++) {
        if (run_multi(exe, del_s, argv0)) {
            break;
        }
    }
    for (i = 0; i < 16; i++) {
        if (run_multi(exe, del_d, argv0)) {
            break;
        }
    }
    run_multi(exe, flush_r, argv0);
    run_multi(exe, flush_s, argv0);
    run_multi(exe, flush_d, argv0);
    run_multi(exe, del_r, argv0);
    run_multi(exe, del_cs, argv0);
    run_multi(exe, del_cd, argv0);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    const char *name;
    char legacy_name[64];
    int backend, res;

    (void) argc;

    if (!argv || !argv[0]) {
        return 1;
    }
    name = base_name(argv[0]);

    if (!strcmp(name, "nft")) {
        load_modules(modules_nft);
        argv[0] = "nft";
        execv(REAL_NFT, argv);
        return 127;
    }

    if (snprintf(legacy_name, sizeof(legacy_name), "%s-legacy", name) < 0) {
        return 1;
    }

    load_modules(modules_iptables);

    if (read_state(name, "legacy")) {
        backend = BACKEND_LEGACY;
    } else if (read_state(name, "nft")) {
        backend = BACKEND_NFT;
    } else {
        backend = BACKEND_UNKNOWN;
    }

    if (backend == BACKEND_LEGACY) {
        res = run_multi(LEGACY_MULTI, argv, legacy_name);
    } else if (backend == BACKEND_NFT) {
        res = run_multi(NFT_MULTI, argv, name);
    } else {
        res = run_multi(NFT_MULTI, argv, name);
        if (!res) {
            backend = BACKEND_NFT;
            write_state(name, "nft");
        } else {
            backend = BACKEND_LEGACY;
            res = run_multi(LEGACY_MULTI, argv, legacy_name);
            if (!res) {
                write_state(name, "legacy");
            }
        }
    }

    if (!res || !has_connbytes(argv)) {
        const char *exe = backend == BACKEND_LEGACY ? LEGACY_MULTI : NFT_MULTI;
        const char *argv0 = backend == BACKEND_LEGACY ? legacy_name : name;

        if (res && is_chain_create(argv)) {
            purge_stale(exe, argv0);
            fprintf(stderr,
                    "fakesip-docker: purged stale FAKESIP chains left by a "
                    "previous run\n");
            res = run_multi(exe, argv, argv0);
        }
        if (!res || !has_connbytes(argv)) {
            return res;
        }
    }

    {
        const char *exe = backend == BACKEND_LEGACY ? LEGACY_MULTI : NFT_MULTI;
        const char *argv0 = backend == BACKEND_LEGACY ? legacy_name : name;

        if (probe_match(exe, argv0, MATCH_CONNBYTES)) {
            return res;
        }
        if (probe_match(exe, argv0, MATCH_HASHLIMIT)) {
            fprintf(stderr,
                    "fakesip-docker: kernel has no connbytes match, "
                    "emulating it with a per-flow hashlimit "
                    "(first N packets of each flow)\n");
            return retry_with(argv, MATCH_HASHLIMIT, exe, argv0);
        }
        if (getenv("FAKESIP_QUEUE") && !strcmp(getenv("FAKESIP_QUEUE"), "all")) {
            fprintf(stderr,
                    "fakesip-docker: kernel has neither connbytes nor "
                    "hashlimit, FAKESIP_QUEUE=all queues every UDP packet\n");
            return retry_with(argv, MATCH_ALL, exe, argv0);
        }
        fprintf(stderr,
                "fakesip-docker: kernel has neither connbytes nor hashlimit, "
                "degrading to \"-m conntrack --ctstate NEW\" "
                "(first packet of each flow only)\n");
        return retry_with(argv, MATCH_CONNTRACK, exe, argv0);
    }
}
