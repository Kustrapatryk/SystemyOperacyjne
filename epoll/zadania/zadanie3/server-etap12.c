#include "l7_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>

#define MAX_CLIENTS 4
#define MAX_EVENTS 10
#define MESSAGE_SIZE 4

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Użycie: %s <PORT>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    uint16_t port = (uint16_t)atoi(argv[1]);
    
    // Tworzenie i bindowanie gniazda nasłuchującego
    int listen_fd = bind_tcp_socket(port, MAX_CLIENTS);

    // Inicjalizacja epoll
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        ERR("epoll_create1");
    }

    struct epoll_event ev;
    ev.events = EPOLLIN; // Interesują nas zdarzenia wejścia (gotowość do odczytu/akceptacji)
    ev.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        ERR("epoll_ctl: listen_fd");
    }

    struct epoll_event events[MAX_EVENTS];
    int active_clients = 0;

    printf("Serwer nasłuchuje na porcie %d...\n", port);

    while (1) {
        // Czekamy na zdarzenia
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue; // Przerwanie przez sygnał, kontynuujemy
            ERR("epoll_wait");
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == listen_fd) {
                // Nowe połączenie od klienta
                int client_fd = add_new_client(listen_fd);
                if (client_fd >= 0) {
                    if (active_clients < MAX_CLIENTS) {
                        // Akceptujemy i dodajemy klienta do epoll
                        ev.events = EPOLLIN;
                        ev.data.fd = client_fd;
                        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                            ERR("epoll_ctl: client_fd");
                        }
                        active_clients++;
                        printf("Nowy posłaniec podłączony. Aktywnych połączeń: %d/%d\n", active_clients, MAX_CLIENTS);
                    } else {
                        // Limit wyczerpany, odrzucamy połączenie
                        printf("Osiągnięto limit (%d) klientów. Odrzucam nowe połączenie.\n", MAX_CLIENTS);
                        close(client_fd);
                    }
                }
            } else {
                // Dane od istniejącego klienta
                int client_fd = events[i].data.fd;
                char buf[MESSAGE_SIZE + 1]; // +1 na znak końca stringa ('\0') dla łatwego wypisania
                
                ssize_t bytes_read = bulk_read(client_fd, buf, MESSAGE_SIZE);

                if (bytes_read == MESSAGE_SIZE) {
                    buf[MESSAGE_SIZE] = '\0'; // Zabezpieczenie stringa
                    printf("Otrzymano wiadomość: %s\n", buf);
                } else if (bytes_read <= 0) {
                    // Mimo braku ścisłego wymogu w etapie 2, podstawowe rozłączanie jest konieczne
                    // aby uniknąć nieskończonej pętli z epoll dla zamkniętego socketu.
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                    close(client_fd);
                    active_clients--;
                    printf("Posłaniec odłączył się z sieci. Aktywnych połączeń: %d/%d\n", active_clients, MAX_CLIENTS);
                }
            }
        }
    }

    // Kod w tym etapie działa w nieskończoność (brak obsługi SIGINT),
    // więc te linie nigdy nie zostaną osiągnięte bez twardego zamknięcia procesu.
    close(listen_fd);
    close(epoll_fd);
    return EXIT_SUCCESS;
}