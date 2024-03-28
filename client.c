#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/wait.h>

#define PORT 5000

#define MAXDATASIZE 100

int main(int argc, char *argv[])
{
    int sockfd, numbytes;
    // char buf[MAXDATASIZE];
    struct hostent *he;
    struct sockaddr_in their_addr;

    int numEntries = 0;
    char buf[64];
    char **entries[64];
    char cmd[64];
    char data_buf[1024];
    char filename[64];

    if (argc != 2)
    {
        fprintf(stderr, "utilizare: client host\n");
        exit(1);
    }

    if ((he = gethostbyname(argv[1])) == NULL)
    {
        perror("gethostbyname");
        exit(1);
    }

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("socket");
        exit(1);
    }

    their_addr.sin_family = AF_INET;
    their_addr.sin_port = htons(PORT);
    their_addr.sin_addr = *((struct in_addr *)he->h_addr);
    memset(&(their_addr.sin_zero), '\0', 8);

    if (connect(sockfd, (struct sockaddr *)&their_addr,
                sizeof(struct sockaddr)) == -1)
    {
        perror("connect");
        exit(1);
    }

    printf("connected to %s\n", argv[1]);

    unsigned char validation_byte = 0;
    do
    {
        // read ls command from stdin
        printf("> ");
        fgets(cmd, 64, stdin);
        cmd[strlen(cmd) - 1] = '\0';

        // send ls command
        if ((send(sockfd, cmd, 64, 0) == -1))
        {
            perror("send: ls command");
        }

        // receive validation 
        recv(sockfd, &validation_byte, 1, 0);

        if (!validation_byte)
        {
            printf("error: invalid ls command\n");
        }

    } while (!validation_byte);

    // receive num of entries
    uint32_t numEntries_nbo = 0;
    if ((numbytes = recv(sockfd, &numEntries_nbo, sizeof(numEntries_nbo), 0)) == -1)
    {
        perror("recv");
        exit(1);
    }

    numEntries = ntohl(numEntries_nbo);

    printf("Number of entries (files): %d\n", numEntries);

    // print entries
    for (int i = 0; i < numEntries; i++)
    {
        uint32_t entry_size_nbo = 0;
        if (recv(sockfd, &entry_size_nbo, sizeof(entry_size_nbo), 0) == -1)
        {
            perror("recv: entry size");
        }
        uint32_t entry_size = ntohl(entry_size_nbo);

        char *entry = calloc(entry_size, sizeof(char));

        if (recv(sockfd, entry, entry_size, 0) == -1)
        {
            perror("recv: entry");
        }

        printf("%s\n", entry);
    }

    // send get command
    validation_byte = 0;
    do
    {
        // read get command from stdin
        printf("> ");
        fgets(cmd, 64, stdin);
        cmd[strlen(cmd) - 1] = '\0';

        // save filename
        strncpy(filename, cmd + 4, strlen(cmd) - 4 + 1);

        if ((send(sockfd, cmd, 64, 0) == -1))
        {
            perror("send: get command");
        }
        
        // receive validation
        recv(sockfd, &validation_byte, 1, 0);

        if (!validation_byte)
        {
            printf("error: invalid get command or filename\n");
        }

    } while (!validation_byte);

    // receive file size
    uint32_t fsize_nbo = 0;
    int fsize = 0;
    if ((recv(sockfd, &fsize_nbo, sizeof(fsize_nbo), 0)) == -1)
    {
        perror("recv: file size");
    }
    fsize = ntohl(fsize_nbo);

    // open dest file for writing
    int write_fd = open(filename, O_WRONLY | O_CREAT, S_IRWXU);
    if (write_fd == -1)
    {
        perror("dest file open");
        exit(EXIT_FAILURE);
    }

    // receive file
    int total_bytes_recv = 0;
    while (total_bytes_recv < fsize)
    {
        int bytes_recv = recv(sockfd, &data_buf, sizeof(data_buf), 0);
        if (bytes_recv == -1)
        {
            perror("recv: file data");
            exit(EXIT_FAILURE);
        }

        if (write(write_fd, data_buf, bytes_recv) != bytes_recv)
        {
            perror("write data");
            exit(EXIT_FAILURE);
        }

        total_bytes_recv += bytes_recv;
    }

    close(write_fd);
    close(sockfd);

    printf("File %s received successfully.\n", filename);

    return 0;
}
