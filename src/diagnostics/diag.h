#pragma once
#include "../common/common.h"

/* Unhandled-exception handler: writes a minidump to logs\crash (newest 5 kept), logs the
 * exception, suppresses the WER dialog. The shell detects the exit and recovers. */
void diag_install_crash_handler(const wchar_t *component);

typedef struct CpuSampler {
    uint64_t lastWall;
    uint64_t lastCpu;
    double   percent;    /* of total machine capacity */
} CpuSampler;

double diag_cpu_process(CpuSampler *s, HANDLE process);
double diag_cpu_job(CpuSampler *s, HANDLE job);
size_t diag_mem_process(HANDLE process);              /* private working set (bytes) */
size_t diag_mem_job(HANDLE job, int *processCount);   /* sum over all processes in the job */
