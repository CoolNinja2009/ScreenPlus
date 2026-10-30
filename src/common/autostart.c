/* Start with Windows: a per-user logon task, the HKCU Run key as the fallback (see autostart.h).
 * The task is registered from XML: a logon trigger for this user only, the user's interactive token
 * (no password, never elevated), normal priority (tasks default to below normal, which also lowers
 * their I/O priority), no time limit and no battery conditions. Task names are machine-wide, so the
 * name carries the user's SID. */
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include "autostart.h"
#include "util.h"
#include <taskschd.h>
#include <sddl.h>

#ifndef AUTOSTART_TASK_PREFIX
#define AUTOSTART_TASK_PREFIX L"ScreenPlus-"      /* + the user's SID */
#endif
#ifndef AUTOSTART_RUN_VALUE
#define AUTOSTART_RUN_VALUE L"ScreenPlus"
#endif
#define RUN_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define APPROVED_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"

/* %ls: the user's SID (trigger), the SID again (principal), the executable and its folder, XML-escaped */
static const wchar_t k_taskXml[] =
    L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
    L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
    L"  <RegistrationInfo><Author>Screen+</Author><Description>Starts Screen+ when you sign in.</Description></RegistrationInfo>\n"
    L"  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>%ls</UserId></LogonTrigger></Triggers>\n"
    L"  <Principals><Principal id=\"Author\"><UserId>%ls</UserId><LogonType>InteractiveToken</LogonType>"
    L"<RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\n"
    L"  <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
    L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
    L"<StartWhenAvailable>false</StartWhenAvailable><RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>"
    L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden>"
    L"<RunOnlyIfIdle>false</RunOnlyIfIdle><WakeToRun>false</WakeToRun><ExecutionTimeLimit>PT0S</ExecutionTimeLimit>"
    L"<Priority>4</Priority></Settings>\n"
    L"  <Actions Context=\"Author\"><Exec><Command>\"%ls\"</Command><Arguments>--startup</Arguments>"
    L"<WorkingDirectory>%ls</WorkingDirectory></Exec></Actions>\n"
    L"</Task>\n";

static BOOL user_sid(wchar_t *out, size_t cap)
{
    union { TOKEN_USER tu; BYTE b[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]; } u;
    HANDLE tok;
    DWORD len = 0;
    wchar_t *s = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return FALSE;
    BOOL ok = GetTokenInformation(tok, TokenUser, &u, (DWORD)sizeof u, &len) && ConvertSidToStringSidW(u.tu.User.Sid, &s);
    CloseHandle(tok);
    if (ok) {
        str_copy(out, cap, s);
        LocalFree(s);
    }
    return ok;
}

static BSTR task_name(void)
{
    wchar_t sid[192], name[224];
    if (!user_sid(sid, SP_COUNTOF(sid))) return NULL;
    str_printf(name, SP_COUNTOF(name), AUTOSTART_TASK_PREFIX L"%ls", sid);
    return SysAllocString(name);
}

/* the scheduler's root folder; the service stays referenced until sched_close */
typedef struct Sched {
    ITaskService *svc;
    ITaskFolder  *root;
} Sched;

static HRESULT sched_open(Sched *s)
{
    VARIANT none;
    VariantInit(&none);
    s->svc = NULL;
    s->root = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskService, (void **)&s->svc);
    if (SUCCEEDED(hr)) hr = ITaskService_Connect(s->svc, none, none, none, none);
    if (SUCCEEDED(hr)) {
        BSTR path = SysAllocString(L"\\");
        hr = path ? ITaskService_GetFolder(s->svc, path, &s->root) : E_OUTOFMEMORY;
        SysFreeString(path);
    }
    if (FAILED(hr) && s->svc) {
        ITaskService_Release(s->svc);
        s->svc = NULL;
    }
    return hr;
}

static void sched_close(Sched *s)
{
    if (s->root) ITaskFolder_Release(s->root);
    if (s->svc) ITaskService_Release(s->svc);
}

/* s with &, < and > escaped for XML text (a folder name can hold &) */
static void xml_text(wchar_t *out, size_t cap, const wchar_t *s)
{
    out[0] = 0;
    for (; *s; s++) {
        wchar_t one[2] = { *s, 0 };
        str_cat(out, cap, *s == L'&' ? L"&amp;" : *s == L'<' ? L"&lt;" : *s == L'>' ? L"&gt;" : one);
    }
}

static HRESULT task_register(const wchar_t *exe)
{
    wchar_t xml[4096];
    wchar_t sid[192], dir[SP_MAX_PATH], exeX[SP_MAX_PATH * 2], dirX[SP_MAX_PATH * 2];
    if (!user_sid(sid, SP_COUNTOF(sid))) return HRESULT_FROM_WIN32(GetLastError());
    str_copy(dir, SP_COUNTOF(dir), exe);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    xml_text(exeX, SP_COUNTOF(exeX), exe);
    xml_text(dirX, SP_COUNTOF(dirX), dir);
    str_printf(xml, SP_COUNTOF(xml), k_taskXml, sid, sid, exeX, dirX);

    BSTR name = task_name(), text = SysAllocString(xml);
    Sched s = { 0 };
    HRESULT hr = name && text ? sched_open(&s) : E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) {
        VARIANT none;
        VariantInit(&none);
        IRegisteredTask *t = NULL;
        hr = ITaskFolder_RegisterTask(s.root, name, text, TASK_CREATE_OR_UPDATE, none, none, TASK_LOGON_INTERACTIVE_TOKEN,
                                      none, &t);
        if (t) IRegisteredTask_Release(t);
        sched_close(&s);
    }
    SysFreeString(text);
    SysFreeString(name);
    return hr;
}

static HRESULT task_delete(void)
{
    BSTR name = task_name();
    Sched s = { 0 };
    HRESULT hr = name ? sched_open(&s) : E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) {
        hr = ITaskFolder_DeleteTask(s.root, name, 0);
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) hr = S_OK;
        sched_close(&s);
    }
    SysFreeString(name);
    return hr;
}

/* the program the task's first action runs, unquoted */
static void task_exe(IRegisteredTask *t, wchar_t *out, size_t cap)
{
    ITaskDefinition *def = NULL;
    IActionCollection *acts = NULL;
    IAction *a = NULL;
    IExecAction *ex = NULL;
    BSTR path = NULL;
    if (SUCCEEDED(IRegisteredTask_get_Definition(t, &def)) && SUCCEEDED(ITaskDefinition_get_Actions(def, &acts)) &&
        SUCCEEDED(IActionCollection_get_Item(acts, 1, &a)) &&
        SUCCEEDED(IAction_QueryInterface(a, &IID_IExecAction, (void **)&ex)) && SUCCEEDED(IExecAction_get_Path(ex, &path)) &&
        path) {
        str_copy(out, cap, path[0] == L'"' ? path + 1 : path);
        wchar_t *q = wcschr(out, L'"');
        if (q) *q = 0;
    }
    SysFreeString(path);
    if (ex) IExecAction_Release(ex);
    if (a) IAction_Release(a);
    if (acts) IActionCollection_Release(acts);
    if (def) ITaskDefinition_Release(def);
}

static BOOL run_query(wchar_t *exe, size_t cap)
{
    wchar_t v[SP_MAX_PATH + 32];
    DWORD size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, AUTOSTART_RUN_VALUE, RRF_RT_REG_SZ, NULL, v, &size) != ERROR_SUCCESS) return FALSE;
    BYTE approved[12];
    size = sizeof approved;
    if (RegGetValueW(HKEY_CURRENT_USER, APPROVED_KEY, AUTOSTART_RUN_VALUE, RRF_RT_REG_BINARY, NULL, approved, &size) == ERROR_SUCCESS &&
        size >= 1 && (approved[0] & 1))
        return FALSE;   /* switched off in Task Manager / Settings */
    if (exe && cap) {   /* "<exe>" --startup */
        int quoted = v[0] == L'"';
        str_copy(exe, cap, v + quoted);
        wchar_t *end = wcschr(exe, quoted ? L'"' : L' ');
        if (end) *end = 0;
    }
    return TRUE;
}

static void run_write(const wchar_t *exe)
{
    wchar_t cmd[SP_MAX_PATH + 32];
    str_printf(cmd, SP_COUNTOF(cmd), L"\"%ls\" --startup", exe);
    RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, AUTOSTART_RUN_VALUE, REG_SZ, cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    RegDeleteKeyValueW(HKEY_CURRENT_USER, APPROVED_KEY, AUTOSTART_RUN_VALUE);   /* back on if Task Manager turned it off */
}

static void run_delete(void)
{
    RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, AUTOSTART_RUN_VALUE);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, APPROVED_KEY, AUTOSTART_RUN_VALUE);
}

AutostartKind autostart_query(wchar_t *exe, size_t cap)
{
    if (exe && cap) exe[0] = 0;
    BSTR name = task_name();
    VARIANT_BOOL on = VARIANT_FALSE;
    Sched s = { 0 };
    if (name && SUCCEEDED(sched_open(&s))) {
        IRegisteredTask *t = NULL;
        if (SUCCEEDED(ITaskFolder_GetTask(s.root, name, &t))) {
            IRegisteredTask_get_Enabled(t, &on);
            if (on && exe && cap) task_exe(t, exe, cap);
            IRegisteredTask_Release(t);
        }
        sched_close(&s);
    }
    SysFreeString(name);
    if (on) return AUTOSTART_TASK;
    return run_query(exe, cap) ? AUTOSTART_RUN : AUTOSTART_OFF;
}

BOOL autostart_set(BOOL on, const wchar_t *exe, AutostartKind *kind, HRESULT *taskHr)
{
    HRESULT hr = on ? task_register(exe) : task_delete();
    if (on && FAILED(hr)) run_write(exe);   /* the slower way in, but a way in */
    else run_delete();                      /* one way in: the task, or none */
    AutostartKind now = autostart_query(NULL, 0);
    if (kind) *kind = now;
    if (taskHr) *taskHr = hr;
    return on ? now != AUTOSTART_OFF : now == AUTOSTART_OFF;
}
