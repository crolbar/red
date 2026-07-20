#include "log.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static FILE* log_file = NULL;

void
log_log_file(const char* level,
             const char* file,
             int         line,
             const char* func,
             const char* fmt,
             ...)
{
    va_list args;
    va_start(args, fmt);
    vargs_log_log_file(level, file, line, func, fmt, args);
    va_end(args);
}

void
vargs_log_log_file(const char* level,
                   const char* file,
                   int         line,
                   const char* func,
                   const char* fmt,
                   va_list     args)
{
    if (!log_file)
        return;

    time_t     now     = time(NULL);
    struct tm* tm_info = localtime(&now);
    char       time_buf[20];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);

    if (strcmp(level, "INFO") == 0)
        fprintf(log_file, ANSI_CYAN);
    if (strcmp(level, "DEBUG") == 0)
        fprintf(log_file, ANSI_BLUE);
    if (strcmp(level, "WARNING") == 0)
        fprintf(log_file, ANSI_YELLOW);
    if (strcmp(level, "ERROR") == 0)
        fprintf(log_file, ANSI_RED);

    if (level)
        fprintf(log_file, "[%s] ", level);

    fprintf(log_file, "[%s] ", time_buf);

    if (strcmp(level, "DEBUG") == 0)
        fprintf(log_file, "%s:%d [%s]: ", file, line, func);

    vfprintf(log_file, fmt, args);

    fprintf(log_file, ANSI_RESET);
    fprintf(log_file, "%s", "\n");
    fflush(log_file);
}

int
create_state_dir()
{
    char* home = getenv("HOME");
    char* fmt  = "%s/.local/state/red";

    int   len  = snprintf(NULL, 0, fmt, home);
    char* path = malloc(len + 1);
    if (!path) {
        return 1;
    }
    sprintf(path, fmt, home, time(NULL));

    struct stat sb;
    if (stat(path, &sb)) {
        mkdir(path, 0755);
    } else {
        if (!S_ISDIR(sb.st_mode)) {
            printf("error: local/state/red not a dir\n");
            return 1;
        }
    }

    free(path);
    return 0;
}

int
open_log_file()
{
    if (create_state_dir())
        return 1;

    // TODO: add timestamp later
    char* home = getenv("HOME");
    // char* fmt = "%s/.local/state/red/log-%d";
    char* fmt = "%s/.local/state/red/log";

    int   len  = snprintf(NULL, 0, fmt, home
                       // , time(NULL)
    );
    char* path = malloc(len + 1);
    if (!path) {
        printf("error: malloc path for log file\n");
        return 1;
    }
    sprintf(path, fmt, home
            // , time(NULL)
    );
    path[len + 1] = '\0';

    int log_fd = open(path, O_CREAT | O_RDWR | O_APPEND, 0644);
    free(path);

    // NOTE: remove later
    int n = ftruncate(log_fd, 0);
    lseek(log_fd, n, SEEK_SET);

    log_file = fdopen(log_fd, "a+");
    if (!log_file) {
        printf("error: creating log file\n");
        return 1;
    }

    return 0;
}

void
print_log_file()
{
    fseek(log_file, 0, SEEK_END);
    long size = ftell(log_file);
    rewind(log_file);

    char* buffer = malloc(size + 1);
    if (!buffer) {
        printf("error: malloc in print log file\n");
        return;
    }

    int n = fread(buffer, 1, size, log_file);
    if (n != size) {
        printf("error: reading log file %d\n", n);
        return;
    }
    buffer[size] = '\0';

    printf("%s\n", buffer);
}

void
close_log_file()
{
    if (log_file)
        fclose(log_file);
}
