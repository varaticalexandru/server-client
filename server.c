#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>

#define MYPORT 5000
#define BACKLOG 10

int main(void)
{
    int sockfd, new_fd;
    struct sockaddr_in my_addr;
    struct sockaddr_in their_addr;
    int sin_size;
    int yes = 1;

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        exit(1);
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
    {
        perror("setsockopt");
        exit(1);
    }

    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(MYPORT);
    my_addr.sin_addr.s_addr = INADDR_ANY;
    memset(&(my_addr.sin_zero), '\0', 8);

    if (bind(sockfd, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) == -1)
    {
        perror("bind");
        exit(1);
    }

    if (listen(sockfd, BACKLOG) == -1)
    {
        perror("listen");
        exit(1);
    }

    while (1)
    {
        pid_t cpid;
        char buf[64];
        char filename[64];
        char fullpath[64];
        char dirpath[64];
        char **entries[64];
        int i = 0;
        int numEntries = 0;
        int read_fd;

        sin_size = sizeof(struct sockaddr_in);
        if ((new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size)) == -1)
        {
            perror("accept");
            continue;
        }
        printf("server: connection from: %s\n", inet_ntoa(their_addr.sin_addr));

        // receive ls command
        char cmd[64];
        unsigned char validation_byte = 0;
        do
        {
            if ((recv(new_fd, &cmd, 64, 0)) == -1)
            {
                perror("recv: ls command");
            }

            // validate ls command
            if (strncmp(cmd, "ls ", 3) == 0)
            {
                validation_byte = 1;
            }

            // send validation
            send(new_fd, &validation_byte, 1, 0);
        } while (!validation_byte);

        // parse dirpath
        strncpy(dirpath, cmd + 3, 64 - 3);
        // add trailing '/' if it doesn't exist
        if (dirpath[strlen(dirpath) - 1] != '/')
        {
            int len = strlen(dirpath);
            dirpath[len] = '/';
            dirpath[len + 1] = '\0';
        }
        printf("dirpath: %s\n", dirpath);

        // pipe
        int pipefd[2];
        if (pipe(pipefd) == -1)
        {
            perror("pipe");
            exit(EXIT_FAILURE);
        }

        // fork
        cpid = fork();
        if (cpid == -1)
        {
            perror("fork");
            exit(EXIT_FAILURE);
        }

        if (cpid == 0)
        { // child
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO); // stdout -> pipe
            close(pipefd[1]);

            // exec ls
            execlp("ls", "ls", "-pA", dirpath, NULL);

            perror("execlp");
            exit(EXIT_FAILURE);
        }
        else
        { // parent
            close(pipefd[1]);

            // each string
            while (read(pipefd[0], &buf[i], 1) > 0)
            {
                if (buf[i] == '\n')
                {
                    buf[i] = '\0';

                    entries[numEntries] = calloc(strlen(buf) + 1, sizeof(char));
                    strcpy(entries[numEntries], buf);

                    // ignore entry if dir (has trailing '/')
                    if (buf[i - 1] == '/')
                    {
                        free(entries[numEntries]);
                        numEntries--;
                    }

                    numEntries++;
                    i = 0;
                }
                else
                {
                    i++;
                }
            }

            // last string
            buf[i] = '\0';
            entries[numEntries] = calloc(strlen(buf) + 1, sizeof(char));
            strcpy(entries[numEntries], buf);

            close(pipefd[0]);
            wait(NULL);
        }

        // send num of entries
        uint32_t numEntries_nbo = htonl(numEntries);
        if (send(new_fd, &numEntries_nbo, sizeof(numEntries_nbo), 0) == -1)
        {
            perror("send: number of entries");
            exit(EXIT_FAILURE);
        }

        // send entries
        for (int i = 0; i < numEntries; i++)
        {
            uint32_t entry_size = htonl(strlen(entries[i]) + 1);

            if (send(new_fd, &entry_size, sizeof(entry_size), 0) == -1)
            {
                perror("send: entry length");
            }

            if (send(new_fd, entries[i], strlen(entries[i]) + 1, 0) == -1)
            {
                perror("send: entry");
            }
        }

        // receive get command
        validation_byte = 0;
        do
        {
            if ((recv(new_fd, &cmd, 64, 0)) == -1)
            {
                perror("recv: get command");
            }

            if (strncmp(cmd, "get ", 4) == 0)
            {
                // get filename
                strncpy(filename, cmd + 4, strlen(cmd) - 4 + 1);
                printf("filename: %s\n", filename);

                // build full path
                strncpy(fullpath, dirpath, strlen(dirpath) + 1);
                strcat(fullpath, filename);
                printf("full path: %s\n", fullpath);

                // open input file
                read_fd = open(fullpath, O_RDONLY);
                if (read_fd != -1)
                {
                    validation_byte = 1;
                }
            }

            send(new_fd, &validation_byte, 1, 0);   // send validation

            if (!validation_byte) {
                printf("%s failed opening\n", fullpath);
            }
        } while (!validation_byte); 

        printf("%s opened successfully\n", fullpath);

        // get file size (bytes)
        struct stat stat_buf;
        fstat(read_fd, &stat_buf);
        size_t fsize = stat_buf.st_size;

        // send file size
        uint32_t fsize_nbo = htonl(fsize);
        if ((send(new_fd, &fsize_nbo, sizeof(fsize_nbo), 0)) == -1)
        {
            perror("send: file size");
        }

        // send file
        off_t offset = 0;
        int sent = 0;
        int size_to_send = fsize;
        while (size_to_send > 0)
        {
            sent = sendfile(new_fd, read_fd, &offset, size_to_send);
            if (sent <= 0)
            {
                perror("sendfile");
                exit(EXIT_FAILURE);
            }
            offset += sent;
            size_to_send -= sent;
        }

        close(read_fd);
        close(new_fd);
        
        printf("File %s sent successfully.\n", filename);
        printf("server: connection closed with: %s\n\n", inet_ntoa(their_addr.sin_addr));
    }

    return 0;
}
