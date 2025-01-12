/*
 * Copyright (c) 2017, 2021, Azul Systems, Inc. All rights reserved.
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define RESTORE_SIGNAL   (SIGRTMIN + 2)

#define PERFDATA_NAME "perfdata"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

#define SUPPRESS_ERROR_IN_PARENT 77

static int g_pid;

static char *verbosity = NULL; // default differs for checkpoint and restore
static char *log_file = NULL;

static int kickjvm(pid_t jvm, int code) {
    union sigval sv = { .sival_int = code };
    if (-1 == sigqueue(jvm, RESTORE_SIGNAL, sv)) {
        perror("sigqueue");
        return 1;
    }
    return 0;
}

static void print_args_to_stderr(const char **args) {
    for (const char **argp = args; *argp != NULL; ++argp) {
        const char *s = *argp;
        if (argp != args) {
            fputc(' ', stderr);
        }
        // https://unix.stackexchange.com/a/357932/296319
        if (!strpbrk(s, " \t\n!\"#$&'()*,;<=>?[\\]^`{|}~")) {
            fputs(s, stderr);
            continue;
        }
        fputc('\'', stderr);
        for (; *s; ++s) {
            if (*s != '\'') {
                fputc(*s, stderr);
            } else {
                fputs("'\\''", stderr);
            }
        }
        fputc('\'', stderr);
    }
}

static void print_command_args_to_stderr(const char **args) {
  fprintf(stderr, "Command: ");
  print_args_to_stderr(args);
  fputc('\n', stderr);
}

static const char *join_path(const char *path1, const char *path2) {
    char *retval;
    if (asprintf(&retval, "%s/%s", path1, path2) == -1) {
        perror("asprintf");
        exit(1);
    }
    return retval;
}

static const char *path_abs(const char *rel) {
    if (rel[0] == '/') {
        return rel;
    }
    char *cwd = get_current_dir_name();
    if (!cwd) {
        perror("get_current_dir_name");
        exit(1);
    }
    return join_path(cwd, rel);
}

static const char *path_abs2(const char *rel1, const char *rel2) {
    if (rel2[0] == '/') {
        return rel2;
    }
    return join_path(path_abs(rel1), rel2);
}

static int checkpoint(pid_t jvm,
        const char *basedir,
        const char *self,
        const char *criu,
        const char *imagedir) {

    if (fork()) {
        // main process
        wait(NULL);
        return 0;
    }

    pid_t parent_before = getpid();

    // child
    if (fork()) {
        exit(0);
    }

    // grand-child
    pid_t parent = getppid();
    int tries = 300;
    while (parent != 1 && 0 < tries--) {
        usleep(10);
        parent = getppid();
    }

    if (parent == parent_before) {
        fprintf(stderr, "can't move out of JVM process hierarchy");
        kickjvm(jvm, -1);
        exit(0);
    }

    char* leave_running = getenv("CRAC_CRIU_LEAVE_RUNNING");

    char jvmpidchar[32];
    snprintf(jvmpidchar, sizeof(jvmpidchar), "%d", jvm);

    const char* args[32] = {
        criu,
        "dump",
        "-t", jvmpidchar,
        "-D", imagedir,
        "--shell-job",
    };
    const char** arg = args + 7;
    *arg++ = verbosity != NULL ? verbosity : "-v4";
    *arg++ = "-o";
    // -D without -W makes criu cd to image dir for logs
    const char *log_local = log_file != NULL ? log_file : "dump4.log";
    *arg++ = log_local;

    if (leave_running) {
        *arg++ = "-R";
    }

    char *criuopts = getenv("CRAC_CRIU_OPTS");
    if (criuopts) {
        char* criuopt = strtok(criuopts, " ");
        while (criuopt && ARRAY_SIZE(args) >= (size_t)(arg - args) + 1/* account for trailing NULL */) {
            *arg++ = criuopt;
            criuopt = strtok(NULL, " ");
        }
        if (criuopt) {
            fprintf(stderr, "Warning: too many arguments in CRAC_CRIU_OPTS (dropped from '%s')\n", criuopt);
        }
    }
    pid_t child = fork();
    *arg++ = NULL;
    if (!child) {
        execv(criu, (char**)args);
        fprintf(stderr, "Cannot execute CRIU \"");
        print_args_to_stderr(args);
        fprintf(stderr, "\": %s\n", strerror(errno));
        exit(SUPPRESS_ERROR_IN_PARENT);
    }

    int status;
    if (child != wait(&status)) {
        fprintf(stderr, "Error waiting for CRIU: %s\n", strerror(errno));
        print_command_args_to_stderr(args);
        kickjvm(jvm, -1);
    } else if (!WIFEXITED(status)) {
        fprintf(stderr, "CRIU has not properly exited, waitpid status was %d - check %s\n", status, path_abs2(imagedir, log_local));
        print_command_args_to_stderr(args);
        kickjvm(jvm, -1);
    } else if (WEXITSTATUS(status)) {
        if (WEXITSTATUS(status) != SUPPRESS_ERROR_IN_PARENT) {
            fprintf(stderr, "CRIU failed with exit code %d - check %s\n", WEXITSTATUS(status), path_abs2(imagedir, log_local));
            print_command_args_to_stderr(args);
        }
        kickjvm(jvm, -1);
    } else if (leave_running) {
        kickjvm(jvm, 0);
    }

    exit(0);
}

static int restore(const char *basedir,
        const char *self,
        const char *criu,
        const char *imagedir) {
    const char* args[32] = {
        criu,
        "restore",
        "-W", ".",
        "--shell-job",
        "--action-script", self,
        "-D", imagedir,
    };
    const char** arg = args + 9;

    *arg++ = verbosity != NULL ? verbosity : "-v1";
    if (log_file != NULL) {
        *arg++ = "-o";
        *arg++ = log_file;
    }

    const char* tail[] = {
        "--exec-cmd", "--", self, "restorewait",
        NULL
    };
    char *criuopts = getenv("CRAC_CRIU_OPTS");
    if (criuopts) {
        char* criuopt = strtok(criuopts, " ");
        while (criuopt && ARRAY_SIZE(args) >= (size_t)(arg - args + ARRAY_SIZE(tail))) {
            *arg++ = criuopt;
            criuopt = strtok(NULL, " ");
        }
        if (criuopt) {
            fprintf(stderr, "Warning: too many arguments in CRAC_CRIU_OPTS (dropped from '%s')\n", criuopt);
        }
    }

    memcpy(arg, tail, sizeof(tail));

    fflush(stderr);

    execv(criu, (char**)args);
    fprintf(stderr, "Cannot execute CRIU \"");
    print_args_to_stderr(args);
    fprintf(stderr, "\": %s\n", strerror(errno));
    return 1;
}

#define MSGPREFIX ""

static int post_resume(void) {
    char *pidstr = getenv("CRTOOLS_INIT_PID");
    if (!pidstr) {
        fprintf(stderr, MSGPREFIX "cannot find CRTOOLS_INIT_PID env\n");
        return 1;
    }
    int pid = atoi(pidstr);

    char *strid = getenv("CRAC_NEW_ARGS_ID");
    return kickjvm(pid, strid ? atoi(strid) : 0);
}

static void sighandler(int sig, siginfo_t *info, void *uc) {
    if (0 <= g_pid) {
        kill(g_pid, sig);
    }
}

static int restorewait(void) {
    char *pidstr = getenv("CRTOOLS_INIT_PID");
    if (!pidstr) {
        fprintf(stderr, MSGPREFIX "no CRTOOLS_INIT_PID: signals may not be delivered\n");
    }
    g_pid = pidstr ? atoi(pidstr) : -1;

    struct sigaction sigact;
    sigfillset(&sigact.sa_mask);
    sigact.sa_flags = SA_SIGINFO;
    sigact.sa_sigaction = sighandler;

    int sig;
    for (sig = 1; sig <= 31; ++sig) {
        if (sig == SIGKILL || sig == SIGSTOP) {
            continue;
        }
        if (-1 == sigaction(sig, &sigact, NULL)) {
            perror("sigaction");
        }
    }

    sigset_t allset;
    sigfillset(&allset);
    if (-1 == sigprocmask(SIG_UNBLOCK, &allset, NULL)) {
        perror(MSGPREFIX "sigprocmask");
    }

    int status;
    int ret;
    do {
        ret = waitpid(g_pid, &status, 0);
    } while (ret == -1 && errno == EINTR);

    if (ret == -1) {
        perror(MSGPREFIX "waitpid");
        return 1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        // Try to terminate the current process with the same signal
        // as the child process was terminated
        const int sig = WTERMSIG(status);
        signal(sig, SIG_DFL);
        raise(sig);
        // Signal was ignored, return 128+n as bash does
        // see https://linux.die.net/man/1/bash
        return 128+sig;
    }

    return 1;
}

// return value is one argument after options
static char *parse_options(int argc, char *argv[]) {
    optind = 2; // starting after action
    struct option opts[] = {{
        .name = "verbosity",
        .has_arg = 1,
        .flag = NULL,
        .val = 'v'
    }, {
        .name = "log-file",
        .has_arg = 1,
        .flag = NULL,
        .val = 'o',
    }, { NULL, 0, NULL, 0} };
    bool processing = true;
    do {
        switch (getopt_long(argc, argv, "v:o:", opts, NULL)) {
            case -1:
            case '?':
                processing = false;
                break;
            case 'v':
                if (asprintf(&verbosity, "--verbosity=%s", optarg) < 0) {
                    fprintf(stderr, "Cannot set verbosity level\n");
                    verbosity = NULL;
                }
                break;
            case 'o':
                log_file = optarg;
                break;
        }
    } while (processing);
    return optind < argc ? argv[optind] : NULL;
}

int main(int argc, char *argv[]) {
    char* action;
    if (argc >= 2 && (action = argv[1])) {

        char* imagedir = parse_options(argc, argv);

        char *basedir = dirname(strdup(argv[0]));

        char *criu = getenv("CRAC_CRIU_PATH");
        if (!criu) {
            if (-1 == asprintf(&criu, "%s/criu", basedir)) {
                return 1;
            }
            struct stat st;
            if (stat(criu, &st)) {
                /* some problem with the bundled criu */
                criu = "/usr/sbin/criu";
                if (stat(criu, &st)) {
                    fprintf(stderr, "cannot find CRIU to use\n");
                    return 1;
                }
            }
        }


        if (!strcmp(action, "checkpoint")) {
            pid_t jvm = getppid();
            return checkpoint(jvm, basedir, argv[0], criu, imagedir);
        } else if (!strcmp(action, "restore")) {
            return restore(basedir, argv[0], criu, imagedir);
        } else if (!strcmp(action, "restorewait")) { // called by CRIU --exec-cmd
            return restorewait();
        } else {
            fprintf(stderr, "unknown command-line action: %s\n", action);
            return 1;
        }
    } else if ((action = getenv("CRTOOLS_SCRIPT_ACTION"))) { // called by CRIU --action-script
        if (!strcmp(action, "post-resume")) {
            return post_resume();
        } else {
            // ignore other notifications
            return 0;
        }
    } else {
        fprintf(stderr, "unknown context\n");
    }

    return 1;
}
