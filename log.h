#pragma once

#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

#define ANSI_RESET "\x1b[0m"
#define ANSI_RED "\x1b[31m"
#define ANSI_YELLOW "\x1b[33m"
#define ANSI_CYAN "\x1b[36m"
#define ANSI_BLUE "\x1b[34m"

int
create_state_dir();

int
open_log_file();

void
print_log_file();

void
close_log_file();

void
log_log_file(const char* level,
             const char* file,
             int         line,
             const char* func,
             const char* fmt,
             ...);

#define ROG(fmt, ...)                                                          \
    log_log_file("DEBUG", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__);
#define ROG_ERR(fmt, ...)                                                      \
    log_log_file("ERROR", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define ROG_WARN(fmt, ...)                                                     \
    log_log_file("WARNING", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define ROG_INFO(fmt, ...)                                                     \
    log_log_file("INFO", __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define ROG_INIT()                                                             \
    if (open_log_file())                                                       \
        return 1;

#define ROG_PRINT_CLOSE()                                                      \
    print_log_file();                                                          \
    ROG_CLOSE()

#define ROG_CLOSE() close_log_file()
