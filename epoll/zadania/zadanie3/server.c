#include "l7_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <signal.h>
#include <stdbool.h>

#define MAX_CLIENTS 4
#define MAX_EVENTS 10
#define MESSAGE_SIZE 4
#define NUM_CITIES 20

// Globalna flaga do bezpiecznego zatrzymania serwera po otrzymaniu SIGINT
volatile sig_atomic_t do_work = 1;

void sigint_handler(int sig) {
    (void)sig; // Ignorowanie ostrzeżenia o nieużywanym parametrze
    do_work = 0;
}

// Funkcja pomocnicza do bezpiecznego odłączania klienta i zwalniania zasobów
void disconnect_client(int epoll_fd, int client_fd, int *clients, int *active_clients) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
    if (close(client_fd) < 0) {
        perror("Błąd zamykania gniazda klienta");
    }
    
    for (int j = 0; j < MAX_CLIENTS; j++) {
        if (clients[j] == client_fd) {
            clients[j] = -1;
            break;
        }
    }
    (*active_clients)--;
    printf("Posłaniec odłączony z sieci. Aktywnych połączeń: %d/%d\n", *active_clients, MAX_CLIENTS);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Użycie: %s <PORT>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    uint16_t port = (uint16_t)atoi(argv[1]);
    
    // Ignorowanie SIGPIPE, aby błąd EPIPE przy write() nie zabijał serwera
    if (sethandler(SIG_IGN, SIGPIPE) == -1) {
        ERR("sethandler SIGPIPE");
    }

    // Konfiguracja obsługi sygnału SIGINT (C-c)
    if (sethandler(sigint_handler, SIGINT) == -1) {
        ERR("sethandler SIGINT");
    }

    // Inicjalizacja stanu miast (1-20). Indeks 0 ignorowany dla czytelności.
    char cities[NUM_CITIES + 1];
    for (int i = 1; i <= NUM_CITIES; i++) {
        cities[i] = 'g'; // Domyślnie na początku wszystkie miasta należą do Greków
    }

    // Śledzenie klientów do rozsyłania wiadomości i zwalniania miejsc
    int clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i] = -1; // -1 oznacza wolny slot
    }
    int active_clients = 0;

    int listen_fd = bind_tcp_socket(port, MAX_CLIENTS);

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        ERR("epoll_create1");
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        ERR("epoll_ctl: listen_fd");
    }

    struct epoll_event events[MAX_EVENTS];

    printf("Biblioteka w Sparcie została otwarta na porcie %d...\n", port);

    while (do_work) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) {
                continue; // Przerwanie sygnałem SIGINT, pętla zakończy się na warunku while(do_work)
            }
            ERR("epoll_wait");
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == listen_fd) {
                // Obsługa nowego połączenia od klienta
                int client_fd = add_new_client(listen_fd);
                if (client_fd >= 0) {
                    if (active_clients < MAX_CLIENTS) {
                        int slot = -1;
                        for (int j = 0; j < MAX_CLIENTS; j++) {
                            if (clients[j] == -1) {
                                slot = j;
                                break;
                            }
                        }

                        if (slot != -1) {
                            ev.events = EPOLLIN;
                            ev.data.fd = client_fd;
                            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                                ERR("epoll_ctl: client_fd");
                            }
                            clients[slot] = client_fd;
                            active_clients++;
                            printf("Nowy posłaniec podłączony. Aktywnych połączeń: %d/%d\n", active_clients, MAX_CLIENTS);
                        }
                    } else {
                        printf("Brak miejsc. Odrzucam nowe połączenie.\n");
                        close(client_fd);
                    }
                }
            } else {
                // Odebrano zdarzenie na deskryptorze istniejącego klienta
                int client_fd = events[i].data.fd;
                char buf[MESSAGE_SIZE + 1];
                
                ssize_t bytes_read = bulk_read(client_fd, buf, MESSAGE_SIZE);

                if (bytes_read == MESSAGE_SIZE) {
                    buf[MESSAGE_SIZE] = '\0';
                    bool valid_format = true;

                    // Weryfikacja formatu wiadomości
                    if ((buf[0] != 'p' && buf[0] != 'g') || 
                        buf[1] < '0' || buf[1] > '9' || 
                        buf[2] < '0' || buf[2] > '9' || 
                        buf[3] != '\n') {
                        valid_format = false;
                    }

                    int city_id = 0;
                    if (valid_format) {
                        city_id = (buf[1] - '0') * 10 + (buf[2] - '0');
                        if (city_id < 1 || city_id > NUM_CITIES) {
                            valid_format = false;
                        }
                    }

                    if (!valid_format) {
                        printf("Otrzymano wadliwą wiadomość od posłańca. Zrywam połączenie.\n");
                        disconnect_client(epoll_fd, client_fd, clients, &active_clients);
                        continue; // Przejście do kolejnego zdarzenia w epoll
                    }

                    // Jeśli format jest poprawny, wypisujemy wiadomość i aktualizujemy stan
                    printf("Posłaniec donosi: %s", buf);
                    char new_owner = buf[0];

                    if (cities[city_id] != new_owner) {
                        cities[city_id] = new_owner;
                        printf("-> Aktualizacja: Miasto %02d zostało przejęte przez %s!\n", 
                               city_id, new_owner == 'g' ? "Greków" : "Persów");

                        // Rozesłanie aktualizacji do innych podłączonych posłańców
                        for (int j = 0; j < MAX_CLIENTS; j++) {
                            if (clients[j] != -1 && clients[j] != client_fd) {
                                ssize_t sent = bulk_write(clients[j], buf, MESSAGE_SIZE);
                                if (sent < 0) {
                                    // Błąd podczas zapisu (np. EPIPE)
                                    printf("Błąd zapisu do posłańca. Odłączam go.\n");
                                    disconnect_client(epoll_fd, clients[j], clients, &active_clients);
                                }
                            }
                        }
                    }
                } else {
                    // bytes_read <= 0 -> 0 oznacza zamknięcie połączenia przez klienta, błąd oznacza błąd odczytu
                    disconnect_client(epoll_fd, client_fd, clients, &active_clients);
                }
            }
        }
    }

    // Zakończenie pracy i raport końcowy po C-c
    printf("\nZamykanie biblioteki... Raport ze stanu miast:\n");
    printf("==================================================\n");
    for (int i = 1; i <= NUM_CITIES; i++) {
        printf("Miasto %02d: %s\n", i, cities[i] == 'g' ? "Grecy" : "Persowie");
    }
    printf("==================================================\n");

    // Zwalnianie zasobów wszystkich połączonych jeszcze posłańców
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] != -1) {
            close(clients[i]);
        }
    }
    close(listen_fd);
    close(epoll_fd);

    printf("Zasoby zwolnione. Koniec programu.\n");
    return EXIT_SUCCESS;
}