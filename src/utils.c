#define _GNU_SOURCE
#include "utils.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int write_to_file(const char *path, const char *content) {
    int fd = open(path, O_WRONLY);

    if (fd == -1) {
        perror("Error opening file");
        printf("  -> Path: %s\n", path);
        return 1;
    }

    ssize_t bytes_written = write(fd, content, strlen(content));

    if (bytes_written == -1) {
        perror("Error writing to file");
        printf("  -> Path: %s, Content: %s\n", path, content);
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}