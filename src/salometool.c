/* salometool.c -- the SALOME service run in the background (see salometool.h). The request goes to
 * <case>/salome/.qc-tool-req.json, the script to <case>/salome/.qc-tool.py, the answer comes back in .qc-tool-ans.json and
 * SALOME's own output in salome/tool.log. OpenFOAM's library paths are kept out of SALOME's environment. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include "app.h"
#include "assets.h"
#include "salometool.h"

static struct {
    pid_t  pid;
    bool   done;                /* finished, its answer not taken yet */
    char   tag[48], dir[PL_PATH];
    double t0;
} R = { .pid = -1 };

void stool_quote(char *out, int cap, const char *s)
{
    int k = 0;
    for (; s && *s && k < cap - 3; s++) {
        if (*s == '"' || *s == '\\') out[k++] = '\\';
        if ((unsigned char)*s < 0x20) continue;
        out[k++] = *s;
    }
    out[k] = 0;
}

bool stool_run(const char *casePath, const char *tag, const char *request)
{
    if (!A.env.salome[0]) { ui_toast(TOAST_ERR, "This needs SALOME (Settings > Programs installs it)"); return false; }
    if (R.pid > 0) { ui_toast(TOAST_INFO, "SALOME is still busy with the last request: one at a time"); return false; }
    char dir[PL_PATH], p[PL_PATH];
    pl_join(dir, sizeof dir, casePath, "salome");
    if (!pl_mkdirs(dir)) { ui_toast(TOAST_ERR, "Could not make %s", dir); return false; }
    pl_join(p, sizeof p, dir, ".qc-tool.py");
    if (!pl_write_file(p, (const char *)asset_salome_tool_py, asset_salome_tool_py_len)) { ui_toast(TOAST_ERR, "Could not write %s", p); return false; }
    pl_join(p, sizeof p, dir, ".qc-tool-req.json");
    if (!pl_write_file(p, request, strlen(request))) { ui_toast(TOAST_ERR, "Could not write %s", p); return false; }
    pl_join(p, sizeof p, dir, ".qc-tool-ans.json");
    unlink(p);
    char qd[PL_PATH + 8], qs[PL_PATH + 8], cmd[PL_PATH * 2 + 400], log[PL_PATH];
    pl_shell_quote(qd, sizeof qd, dir);
    pl_shell_quote(qs, sizeof qs, A.env.salome);
    if (!pl_fmt(cmd, sizeof cmd, "cd %s && env -u LD_LIBRARY_PATH -u PYTHONPATH -u PYTHONHOME QC_CMD=\"$PWD/.qc-tool-req.json\" "
                                 "QC_OUT=\"$PWD/.qc-tool-ans.json\" %s -t --shutdown-servers=1 .qc-tool.py", qd, qs)) {
        ui_toast(TOAST_ERR, "The case path is too long to start SALOME from"); return false;
    }
    pl_join(log, sizeof log, dir, "tool.log");
    R.pid = pl_spawn_shell(cmd, log);
    if (R.pid <= 0) { R.pid = -1; ui_toast(TOAST_ERR, "Could not start SALOME"); return false; }
    pl_strlcpy(R.tag, tag, sizeof R.tag);
    pl_strlcpy(R.dir, dir, sizeof R.dir);
    R.done = false;
    R.t0 = GetTime();
    return true;
}

bool stool_busy(const char *tag) { return R.pid > 0 && (!tag || !strcmp(tag, R.tag)); }
double stool_seconds(void) { return R.pid > 0 ? GetTime() - R.t0 : 0; }

void stool_tick(void)
{
    if (R.pid <= 0) return;
    ui_request_frames(2);
    int st;
    if (waitpid(R.pid, &st, WNOHANG) != R.pid) return;
    R.pid = -1;
    R.done = true;
}

QJ *stool_take(const char *tag)
{
    if (!R.done || strcmp(tag, R.tag)) return NULL;
    R.done = false;
    char p[PL_PATH], err[200];
    pl_join(p, sizeof p, R.dir, ".qc-tool-ans.json");
    char *t = pl_read_file(p, NULL);
    QJ *j = t ? qj_parse(t, err, sizeof err) : NULL;
    free(t);
    if (j) return j;
    /* no answer: SALOME died before writing it; its log's last line is the best reason there is */
    char lp[PL_PATH], last[300] = "SALOME stopped without an answer";
    pl_join(lp, sizeof lp, R.dir, "tool.log");
    char *l = pl_read_file(lp, NULL);
    if (l) {
        for (char *line = strtok(l, "\n"); line; line = strtok(NULL, "\n")) if (line[strspn(line, " \t\r")]) pl_strlcpy(last, line, sizeof last);
        free(l);
    }
    char q[400], json[500];
    stool_quote(q, sizeof q, last);
    snprintf(json, sizeof json, "{\"ok\": false, \"error\": \"%s (salome/tool.log has it all)\"}", q);
    return qj_parse(json, err, sizeof err);
}
