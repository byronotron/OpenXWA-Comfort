#ifndef XWA_APP_CRASH_REPORT_H
#define XWA_APP_CRASH_REPORT_H

/* Writes a native crash report (fault + backtrace) to `path` when the
 * process dies on a fatal signal. No-op on platforms without support. */
void XwaCrashReport_Install(const char* path);

#endif
