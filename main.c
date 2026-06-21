#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define SWS_VERSION "0.0.1"
#define SERVER_PORT 4545
#define MAX_REQUESTS_QUEUE_LENGHT 10

int start_http_server(void)
{
    int server_fd, client_socket_fd;
    struct sockaddr_in server_addr;
    int addrlen = sizeof(server_addr);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0)
    {
        perror("ERROR | socket initialization failed");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(server_fd, MAX_REQUESTS_QUEUE_LENGHT);

    printf("INFO | server listening at %d...\n", SERVER_PORT);

    while (1)
    {
        client_socket_fd = accept(server_fd, (struct sockaddr *)&server_addr, (socklen_t *)&addrlen);

        if (client_socket_fd < 0)
        {
            perror("ERROR | fail to accept connection");
            continue;
        }

        char buffer[1024] = {0};
        read(client_socket_fd, buffer, 1024);
        printf("INFO | request received...\n%s\n", buffer);

        char *http_response = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Length: 12\r\n"
                              "\r\n"
                              "Hello World!";

        write(client_socket_fd, http_response, strlen(http_response));
        close(client_socket_fd);
    }

    exit(EXIT_SUCCESS);
}

void show_welcome_message(void)
{
    printf("\n");
    printf("=============================================\n");
    printf("SWS - Simple Web Server %s\n", SWS_VERSION);
    printf("=============================================\n");
    printf("\n");
}

int main(void)
{
    show_welcome_message();
    start_http_server();
    exit(EXIT_SUCCESS);
}
