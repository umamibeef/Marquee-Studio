#ifndef STUB_TIME_H
#define STUB_TIME_H
typedef long time_t;
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
time_t time(); struct tm *gmtime(); struct tm *localtime(); long clock();
#define CLOCKS_PER_SEC 1000000
#endif
