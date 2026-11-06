/* Threads, events and processes behind Win32's HANDLE calls, so the shared workers (store.c, depth.c, update.c,
 * samples_update.c) build unchanged. File handles are small numbers (compat.c); these are pointers. Waits poll every
 * few milliseconds: they only happen on worker threads, during jobs. */
#include "compat.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

enum { OBJ_THREAD = 0x53504f31, OBJ_EVENT, OBJ_PROCESS };

typedef struct Obj {
    int        kind, refs;
    volatile int done;              /* a thread finished; an event is set; a process exited */
    int        manual;              /* event: stays set */
    DWORD      code;
    pid_t      pid;
    pthread_t  thread;
    LPTHREAD_START_ROUTINE fn;
    void      *arg;
} Obj;

static Obj *obj_new(int kind)
{
    Obj *o = (Obj *)calloc(1, sizeof *o);
    if (o) { o->kind = kind; o->refs = 1; }
    return o;
}

static void obj_unref(Obj *o)
{
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST) == 0) free(o);
}

int sp_obj_close(HANDLE h)
{
    if ((uintptr_t)h < (1u << 24)) return 0;       /* a file */
    Obj *o = (Obj *)h;
    if (o->kind == OBJ_PROCESS && !o->done) {
        int st;
        if (waitpid(o->pid, &st, WNOHANG) == 0) {  /* still running: let it go, it isn't ours to wait for any more */
            obj_unref(o);
            return 1;
        }
    }
    obj_unref(o);
    return 1;
}

static void *thread_main(void *p)
{
    Obj *o = (Obj *)p;
    o->code = o->fn(o->arg);
    __atomic_store_n(&o->done, 1, __ATOMIC_SEQ_CST);
    obj_unref(o);
    return NULL;
}

HANDLE CreateThread(void *sec, size_t stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *id)
{
    Obj *o = obj_new(OBJ_THREAD);
    if (!o) return NULL;
    o->fn = fn;
    o->arg = arg;
    o->refs = 2;                                   /* the caller's handle and the thread itself */
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&o->thread, &a, thread_main, o);
    pthread_attr_destroy(&a);
    if (rc) { free(o); return NULL; }
    if (id) *id = 0;
    return o;
}

HANDLE CreateEventW(void *sec, BOOL manual, BOOL initial, const wchar_t *name)
{
    Obj *o = obj_new(OBJ_EVENT);
    if (o) { o->manual = manual; o->done = initial; }
    return o;
}

BOOL SetEvent(HANDLE h)   { __atomic_store_n(&((Obj *)h)->done, 1, __ATOMIC_SEQ_CST); return TRUE; }
BOOL ResetEvent(HANDLE h) { __atomic_store_n(&((Obj *)h)->done, 0, __ATOMIC_SEQ_CST); return TRUE; }

static int signalled(Obj *o)
{
    if (o->kind == OBJ_PROCESS && !o->done) {
        int st;
        pid_t r = waitpid(o->pid, &st, WNOHANG);
        if (r == o->pid || (r < 0 && errno == ECHILD)) {
            o->code = r == o->pid && WIFEXITED(st) ? (DWORD)WEXITSTATUS(st) : (DWORD)255;
            o->done = 1;
        }
    }
    if (!__atomic_load_n(&o->done, __ATOMIC_SEQ_CST)) return 0;
    if (o->kind == OBJ_EVENT && !o->manual) o->done = 0;
    return 1;
}

DWORD WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms)
{
    struct timespec start, now, nap = { 0, 5000000 };
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        DWORD set = 0;
        for (DWORD i = 0; i < n; i++) {
            if (!signalled((Obj *)h[i])) continue;
            if (!all) return WAIT_OBJECT_0 + i;
            set++;
        }
        if (all && set == n) return WAIT_OBJECT_0;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double gone = (now.tv_sec - start.tv_sec) * 1000.0 + (now.tv_nsec - start.tv_nsec) / 1e6;
        if (ms != INFINITE && gone >= ms) return WAIT_TIMEOUT;
        nanosleep(&nap, NULL);
    }
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return WaitForMultipleObjects(1, &h, FALSE, ms); }
void  Sleep(DWORD ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 }; nanosleep(&t, NULL); }

BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *c, SRWLOCK *l, DWORD ms, ULONG flags)
{
    pthread_mutex_lock(&c->m);
    unsigned seen = c->seq;                         /* taken with l held: a wake from here on is not lost */
    pthread_rwlock_unlock(l);
    while (c->seq == seen) pthread_cond_wait(&c->c, &c->m);
    pthread_mutex_unlock(&c->m);
    pthread_rwlock_wrlock(l);
    return TRUE;
}

void WakeAllConditionVariable(CONDITION_VARIABLE *c)
{
    pthread_mutex_lock(&c->m);
    c->seq++;
    pthread_cond_broadcast(&c->c);
    pthread_mutex_unlock(&c->m);
}

void WakeConditionVariable(CONDITION_VARIABLE *c) { WakeAllConditionVariable(c); }

/* Splits a Windows command line: "quoted parts" and plain words. */
static int split_args(char *s, char **argv, int max)
{
    int n = 0;
    while (*s && n < max - 1) {
        while (*s == ' ') s++;
        if (!*s) break;
        char *out = s;
        argv[n++] = s;
        int q = 0;
        for (; *s && (q || *s != ' '); s++) {
            if (*s == '"') q = !q;
            else if (*s == '\\' && s[1] == '"') *out++ = *++s;
            else *out++ = *s;
        }
        if (*s) s++;
        *out = 0;
    }
    argv[n] = NULL;
    return n;
}

BOOL CreateProcessW(const wchar_t *app, wchar_t *cmd, void *a, void *b, BOOL inherit, DWORD flags, void *env, const wchar_t *dir,
                    STARTUPINFOW *si, PROCESS_INFORMATION *pi)
{
    char *line = sp_utf8_dup(cmd), *argv[256];
    if (!line) return FALSE;
    int n = split_args(line, argv, 256);
    if (!n) { free(line); return FALSE; }
    size_t l = strlen(argv[0]);
    if (l > 4 && !strcmp(argv[0] + l - 4, ".exe")) argv[0][l - 4] = 0;
    char *base = strrchr(argv[0], '/');
    if (base && !strcmp(base, "/tar") && !access("/usr/bin/bsdtar", X_OK)) argv[0] = (char *)"/usr/bin/bsdtar";   /* Windows' tar reads zips */
    char *cwd = dir ? sp_path_utf8(dir) : NULL;
    int nice_by = flags & BELOW_NORMAL_PRIORITY_CLASS ? 10 : 0;
    pid_t pid = fork();                            /* few enough, and vfork-unsafe chdir/nice follow */
    if (pid == 0) {
        if (cwd && chdir(cwd)) _exit(127);
        if (nice_by && nice(nice_by) == -1) {}
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0); dup2(null, 1); dup2(null, 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    free(line);
    free(cwd);
    if (pid < 0) return FALSE;
    Obj *o = obj_new(OBJ_PROCESS);
    if (!o) return FALSE;
    o->pid = pid;
    pi->hProcess = o;
    pi->hThread = obj_new(OBJ_EVENT);
    pi->dwProcessId = (DWORD)pid;
    return TRUE;
}

BOOL TerminateProcess(HANDLE h, UINT code) { return kill(((Obj *)h)->pid, SIGKILL) == 0; }

BOOL GetExitCodeProcess(HANDLE h, DWORD *code)
{
    Obj *o = (Obj *)h;
    signalled(o);
    *code = o->done ? o->code : 259;               /* STILL_ACTIVE */
    return TRUE;
}

UINT GetSystemDirectoryW(wchar_t *out, UINT cap)
{
    wcsncpy(out, L"/usr/bin", cap);
    return 8;
}
