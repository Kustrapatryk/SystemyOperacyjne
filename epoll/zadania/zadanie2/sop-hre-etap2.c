#include "l4-common.h"

#define BACKLOG_SIZE 10
#define MAX_EVENTS 16
#define BUF_SIZE 256

void usage(char* name)
{
    fprintf(stderr, "USAGE: %s port\n", name);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv) { 
    if (argc != 2) {
        usage(argv[0]);
    }

    uint16_t port = (uint16_t)atoi(argv[1]);
    int listen_socket = bind_tcp_socket(port, BACKLOG_SIZE);

    // 1. Inicjalizacja mechanizmu epoll
    int epoll_descriptor;
    if ((epoll_descriptor = epoll_create1(0)) < 0) {
        ERR("epoll_create1");
    }

    // Dodanie gniazda nasłuchującego do epoll
    struct epoll_event event, events[MAX_EVENTS];
    event.events = EPOLLIN;
    event.data.fd = listen_socket;
    if (epoll_ctl(epoll_descriptor, EPOLL_CTL_ADD, listen_socket, &event) == -1) {
        ERR("epoll_ctl: listen_socket");
    }

    printf("Serwer uruchomiony na porcie %d. Oczekuję na elektorów...\n", port);

    // 2. Główna pętla obsługująca zdarzenia
    while (1) {
        int nfds = epoll_wait(epoll_descriptor, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue; // Przerywane przez sygnał, kontynuujemy
            ERR("epoll_wait");
        }

        for (int n = 0; n < nfds; n++) {
            if (events[n].data.fd == listen_socket) {
                // Nowe połączenie od klienta
                int client_socket = add_new_client(listen_socket);
                if (client_socket == -1) continue; // Zabezpieczenie przed EAGAIN

                printf("-> Nowy elektor połączony (deskryptor: %d)\n", client_socket);

                // Wysłanie wiadomości powitalnej
                char *welcome_msg = "Welcome, elector!\n";
                if (bulk_write(client_socket, welcome_msg, strlen(welcome_msg)) < 0) {
                    perror("bulk_write welcome");
                }

                // Zarejestrowanie nowego klienta w epoll (aby nasłuchiwać co pisze)
                event.events = EPOLLIN;
                event.data.fd = client_socket;
                if (epoll_ctl(epoll_descriptor, EPOLL_CTL_ADD, client_socket, &event) == -1) {
                    perror("epoll_ctl: add client");
                    close(client_socket);
                }
            } else {
                // Istniejący klient przysłał dane
                int client_socket = events[n].data.fd;
                char buf[BUF_SIZE];
                
                // Zwykły read czyta dowolną ilość znaków do limitu bufora
                ssize_t size = TEMP_FAILURE_RETRY(read(client_socket, buf, BUF_SIZE - 1));
                
                if (size < 0) {
                    // Wystąpił błąd odczytu
                    perror("read z klienta");
                    epoll_ctl(epoll_descriptor, EPOLL_CTL_DEL, client_socket, NULL);
                    TEMP_FAILURE_RETRY(close(client_socket));
                } else if (size == 0) {
                    // Klient się rozłączył poprawnie (Zgrabne rozłączenie)
                    printf("<- Elektor rozłączony (deskryptor: %d)\n", client_socket);
                    epoll_ctl(epoll_descriptor, EPOLL_CTL_DEL, client_socket, NULL);
                    TEMP_FAILURE_RETRY(close(client_socket));
                } else {
                    // Odebrano wiadomość
                    buf[size] = '\0'; // Bezpieczne zakończenie stringa
                    
                    // Wypisanie wiadomości na stdout
                    printf("Wiadomość od (fd: %d): %s", client_socket, buf);
                    
                    // Netcat wysyła '\n' na końcu po wciśnięciu Entera. 
                    // Jeśli go brakuje, dokładamy, by logi były czytelne.
                    if (buf[size-1] != '\n') printf("\n");
                }
            }
        }
    }

    // Kod nigdy tu nie dotrze, ale zamykamy formalnie zasoby 
    // (prawidłowe zamykanie przez sygnały dodamy w etapie 5)
    close(epoll_descriptor);
    close(listen_socket);
    return EXIT_SUCCESS;
}