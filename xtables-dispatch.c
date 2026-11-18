/*
 * xtables-dispatch.c - FakeSIP-Docker
 *
 * Small shim installed as /usr/sbin/iptables and /usr/sbin/ip6tables.
 *
 * It prefers the nft-backed xtables binary and transparently falls back to the
 * legacy (ip_tables / ip6_tables) one when nf_tables is unavailable or broken
 * (e.g. Synology DSM 4.4), and vice versa when only legacy is broken.
 *
 * The chosen backend is cached per command name, so a single run never mixes
 * both backends for the same address family.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define STATE_DIR "/run"
#define NFT_MULTI "/usr/sbin/xtables-nft-multi"
#define LEGACY_MULTI "/usr/sbin/xtables-legacy-multi"

enum { BACKEND_UNKNOWN = 0, BACKEND_NFT = 1, BACKEND_LEGACY = 2 };

static const char *base_name(const char *path)
{
    const char *slash;

    if (!path || !*path) {
        return "iptables";
    }
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int read_backend(const char *name)
{
    char path[128], buf[16];
    FILE *fp;

    if (snprintf(path, sizeof(path), STATE_DIR "/xtables.%s", name) < 0) {
        return BACKEND_UNKNOWN;
    }
    fp = fopen(path, "r");
    if (!fp) {
        return BACKEND_UNKNOWN;
    }
    if (!fgets(buf, sizeof(buf), fp)) {
        buf[0] = '\0';
    }
    fclose(fp);

    if (!strncmp(buf, "legacy", 6)) {
        return BACKEND_LEGACY;
    }
    if (!strncmp(buf, "nft", 3)) {
        return BACKEND_NFT;
    }
    return BACKEND_UNKNOWN;
}

static void write_backend(const char *name, int legacy)
{
    char path[128];
    FILE *fp;

    if (snprintf(path, sizeof(path), STATE_DIR "/xtables.%s", name) < 0) {
        return;
    }
    fp = fopen(path, "w");
    if (!fp) {
        return;
    }
    fputs(legacy ? "legacy\n" : "nft\n", fp);
    fclose(fp);
}

static void clear_backend(const char *name)
{
    char path[128];

    if (snprintf(path, sizeof(path), STATE_DIR "/xtables.%s", name) < 0) {
        return;
    }
    unlink(path);
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
    if (snprintf(legacy_name, sizeof(legacy_name), "%s-legacy", name) < 0) {
        return 1;
    }

    backend = read_backend(name);
    if (backend == BACKEND_LEGACY) {
        return run_multi(LEGACY_MULTI, argv, legacy_name);
    }
    if (backend == BACKEND_NFT) {
        return run_multi(NFT_MULTI, argv, name);
    }

    res = run_multi(NFT_MULTI, argv, name);
    if (!res) {
        write_backend(name, 0);
        return 0;
    }

    res = run_multi(LEGACY_MULTI, argv, legacy_name);
    if (!res) {
        write_backend(name, 1);
        return 0;
    }

    clear_backend(name);
    return res;
}
