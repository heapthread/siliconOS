// silicon - process shell commands: ps, kill, run
#include "shell.h"
#include "../proc/proc.h"

void uapp_run(const char* path);

void cmd_ps(Shell& sh, int, char**) {
    sh.out(C_BWHITE "  pid  threads  memory    cpu      name\n" C_RESET);
    sh.outf("   0        1  kernel    %3d.%d%%   silicon (the kernel thread)\n", 100 - proc_cpu_permille() / 10, (1000 - proc_cpu_permille()) % 10 > 0 ? 0 : 0);
    for (int i = 0; i < proc_count(); i++) {
        Proc* p = proc_at(i);
        u64 up = ticks() - p->start_ms + 1;
        u64 permille = p->cpu_tsc / max<u64>(1, (u64)cpu_mhz() * up);
        sh.outf("  %3d  %7d  %5u KB  %3u.%u%%   %s%s\n", p->pid, p->threads.len, p->mem_pages * 4, (u32)(permille / 10), (u32)(permille % 10), p->name,
                p->dead ? C_GRAY "  (ended)" C_RESET : "");
    }
}

void cmd_kill(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: kill <pid>\n"); return; }
    Proc* p = proc_by_pid(atoi(argv[1]));
    if (!p || p->dead) { sh.err("no such process\n"); return; }
    proc_kill(p, "killed");
    sh.outf("process %d (%s) ended\n", p->pid, p->name);
}

void cmd_run(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: run <program.uapp>      (see sdk/build_uapp.py; samples are in /home/apps)\n"); return; }
    char path[200];
    if (argv[1][0] == '/') strlcpy(path, argv[1], sizeof(path));
    else { char cwd[160]; fs_path(sh.cwd, cwd, sizeof(cwd)); snprintf(path, sizeof(path), "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", argv[1]); }
    char err[96];
    Proc* p = proc_spawn(path, err, sizeof(err));
    if (!p) { sh.err("%s\n", err); return; }
    sh.outf("started %s as process %d (it opens its own window in the desktop)\n", p->name, p->pid);
}
