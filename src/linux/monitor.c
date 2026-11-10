#include "../monitor/monitor.h"
#include "../common/util.h"

const MonitorInfo *monitor_find(const MonitorInfo *list, int count, const wchar_t *id)
{
    if (!list || !id) return NULL;
    for (int i = 0; i < count; i++)
        if (str_ieq(list[i].id, id)) return &list[i];
    return NULL;
}
