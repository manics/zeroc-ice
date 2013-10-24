#include <cstdio>
#include <ctime>
#include <unistd.h>



static void tmpLog(const char* file, const int line, const char* func, const char* message = "")
{
    time_t now = time(0);
    tm ts;
    localtime_r(&now, &ts);
    char dtstr[128];
    strftime(dtstr, 128, "%F %T", &ts);

    char name[1024];
    sprintf(name, "tmp-ice-cpy.log.%05d", getpid());
    FILE *f = fopen(name, "a");
    fprintf(f, "%s %s:%d:%s %s\n", dtstr, file, line, func, message);
    fclose(f);
}

#define TMPLOGF tmpLog(__FILE__, __LINE__, __func__);
#define TMPLOG(x) tmpLog(__FILE__, __LINE__, __func__, x);
