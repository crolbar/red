#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "unknown"
#endif

int
connect_to_socket()
{
    char* sock_path = getenv("RED_SOCKET");
    if (!sock_path) {
        printf("RED_SOCKET not set!\n");
        return -1;
    }

    struct sockaddr_un addr = {};
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, sock_path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        printf("failed to create socket: %s\n", strerror(errno));
        goto fail;
    }
    if (connect(fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un))) {
        printf("failed to connect to socket at %s: %s\n",
               sock_path,
               strerror(errno));
        close(fd);
        goto fail;
    }
    return fd;
fail:
    return -1;
}

int
construct_msg(int argc, char** argv, char** out_msg)
{
    char* msg = NULL;
    if (argc == 1)
        return 0;
    size_t msg_len = argc - 2;
    for (int i = 1; i < argc; i++)
        msg_len += strlen(argv[i]);
    msg = malloc(msg_len + 1);
    if (!msg)
        goto fail;

    for (size_t i = 0; i < msg_len;) {
        for (int ai = 1; ai < argc; ai++) {
            char*  arg     = argv[ai];
            size_t arg_len = strlen(arg);
            for (size_t j = 0; j < arg_len; j++)
                msg[i++] = arg[j];

            if (i != msg_len - 1)
                msg[i++] = ' ';
        }
    }

    msg[msg_len] = '\0';

    *out_msg = msg;
    return msg_len;
fail:
    free(msg);
    return -1;
}

int
main(int argc, char** argv)
{
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "version") == 0 ||
            strcmp(argv[i], "--version") == 0) {
            printf("version: %s\n", VERSION);
            return 0;
        }

        if (strcmp(argv[i], "help") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("help menu TODO\n");
            return 0;
        }
    }

    int sock_fd = connect_to_socket();
    if (sock_fd == -1)
        return -1;

    char* msg     = NULL;
    int   msg_len = 0;
    if ((msg_len = construct_msg(argc, argv, &msg)) <= 0) {
        if (msg_len < 0)
            printf("falied to construct msg\n");
        else if (msg_len == 0)
            printf("no args provided\n");
        return 1;
    }

    if (write(sock_fd, msg, msg_len) == -1) {
        printf("write error: %s\n", strerror(errno));
        return 1;
    }

    {
        char buf[248];
    read:
        memset(buf, 0, sizeof(buf));
        if (read(sock_fd, buf, sizeof(buf)) == -1) {
            printf("read error: %s\n", strerror(errno));
            return 1;
        }

        printf("%s", buf);

        if (poll(&(struct pollfd){ sock_fd, POLLIN, 0 }, 1, 0) > 0)
            goto read;
        printf("\n");
    }

    free(msg);
    return 0;
}
