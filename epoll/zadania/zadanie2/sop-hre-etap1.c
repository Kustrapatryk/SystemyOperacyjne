#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "l4-common.h"

#define BACKLOG_SIZE 1

void usage(char* name)
{
    fprintf(stderr, "USAGE: %s port\n", name);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv) { 
    
    if (argc != 2) {
        usage(argv[0]);
    }

    int port = atoi(argv[1]);

    
    // Utworzenie gniazda TCP i rozpoczęcie nasłuchiwania
    int listen_socket = bind_tcp_socket(port, BACKLOG_SIZE);

    // Oczekiwanie na pojedyncze połączenie TCP
    int client_socket = add_new_client(listen_socket);
    if (client_socket < 0) {
        ERR("add_new_client");
    }

    // Wypisanie komunikatu po nawiązaniu połączenia
    printf("Klient połączony\n");

    // Zamknięcie połączenia z klientem
    if (TEMP_FAILURE_RETRY(close(client_socket)) < 0) {
        ERR("close client_socket");
    }

    // Zamknięcie gniazda nasłuchującego serwera
    if (TEMP_FAILURE_RETRY(close(listen_socket)) < 0) {
        ERR("close listen_socket");
    }

    return EXIT_SUCCESS;

}
