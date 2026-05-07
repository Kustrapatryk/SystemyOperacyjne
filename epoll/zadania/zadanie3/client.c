#include "l7_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <signal.h>
#include <time.h>
#include <stdbool.h>

#define NUM_CITIES 20
#define MAX_EVENTS 2
#define MESSAGE_SIZE 4

// Zmienna globalna do kontrolowania głównej pętli przez sygnał
volatile sig_atomic_t do_work = 1;

void sigint_handler(int sig) {
    (void)sig; // Ignorujemy ostrzeżenie o nieużywanym argumencie
    do_work = 0;
}

// Funkcja pomocnicza do wypisywania stanu miast
void print_owners(const char *cities) {
    printf("\n--- Stan Własności Miast ---\n");
    for (int i = 1; i <= NUM_CITIES; i++) {
        printf("Miasto %02d: ", i);
        if (cities[i] == 'g') {
            printf("Grecy\n");
        } else if (cities[i] == 'p') {
            printf("Persowie\n");
        } else {
            printf("Nieznany (?)\n");
        }
    }
    printf("----------------------------\n");
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "Użycie: %s <ADRES SERWERA> <NUMER PORTU>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *server_ip = argv[1];
    char *server_port = argv[2];

    // Ustawienie obsługi sygnału SIGINT (C-c)
    if (sethandler(sigint_handler, SIGINT) == -1) {
        ERR("sethandler");
    }

    // Inicjalizacja generatora liczb pseudolosowych
    srand(time(NULL) ^ getpid());

    // Inicjalizacja tablicy miast (indeksy 1-20, wartość '?' oznacza stan nieznany)
    char cities[NUM_CITIES + 1];
    for (int i = 1; i <= NUM_CITIES; i++) {
        cities[i] = '?';
    }

    // Nawiązanie połączenia z serwerem
    int sock = connect_tcp_socket(server_ip, server_port);
    printf("Połączono z biblioteką w Sparcie (%s:%s).\n", server_ip, server_port);
    printf("Dostępne komendy:\n"
           "  e      - wyjdź\n"
           "  m XXX  - wyślij 3 znaki do serwera\n"
           "  t XX   - podróż i aktualizacja miasta (XX = 01..20)\n"
           "  o      - wypisz właścicieli miast\n\n");

    // Konfiguracja epoll
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        ERR("epoll_create1");
    }

    struct epoll_event ev, events[MAX_EVENTS];

    // Dodanie STDIN do monitorowania
    ev.events = EPOLLIN;
    ev.data.fd = STDIN_FILENO;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, STDIN_FILENO, &ev) == -1) {
        ERR("epoll_ctl: stdin");
    }

    // Dodanie gniazda sieciowego do monitorowania
    ev.events = EPOLLIN;
    ev.data.fd = sock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev) == -1) {
        ERR("epoll_ctl: sock");
    }

    while (do_work) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) {
                continue; // Przerwane przez sygnał SIGINT
            }
            ERR("epoll_wait");
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == STDIN_FILENO) {
                // Odczyt komend z klawiatury
                char buf[256];
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
                
                if (n > 0) {
                    buf[n] = '\0'; // Bezpieczne zamknięcie stringa
                    
                    if (buf[0] == 'e') {
                        // Zakończenie pracy
                        do_work = 0;
                    } 
                    else if (buf[0] == 'm' && buf[1] == ' ') {
                        // Komenda 'm XXX'
                        char msg[MESSAGE_SIZE];
                        msg[0] = (n > 2 && buf[2] != '\n') ? buf[2] : ' ';
                        msg[1] = (n > 3 && buf[3] != '\n') ? buf[3] : ' ';
                        msg[2] = (n > 4 && buf[4] != '\n') ? buf[4] : ' ';
                        msg[3] = '\n';
                        
                        bulk_write(sock, msg, MESSAGE_SIZE);
                        printf("Wysłano wiadomość tekstową.\n");
                    } 
                    else if (buf[0] == 't' && buf[1] == ' ') {
                        // Komenda 't XX'
                        int city = atoi(&buf[2]);
                        if (city >= 1 && city <= NUM_CITIES) {
                            // Losowanie właściciela: g lub p
                            char faction = (rand() % 2 == 0) ? 'g' : 'p';
                            
                            char msg[MESSAGE_SIZE + 1]; 
                            snprintf(msg, sizeof(msg), "%c%02d\n", faction, city);
                            
                            // Wysłanie dokładnie 4 bajtów do serwera
                            bulk_write(sock, msg, MESSAGE_SIZE);
                            
                            // Aktualizacja własnej bazy
                            cities[city] = faction;
                            printf("Podróż: Ustalono, że miasto %02d jest kontrolowane przez %s.\n", 
                                   city, faction == 'g' ? "Greków" : "Persów");
                        } else {
                            printf("Błąd: Numer miasta (%d) poza zakresem [1, 20].\n", city);
                        }
                    } 
                    else if (buf[0] == 'o') {
                        // Komenda 'o'
                        print_owners(cities);
                    }
                } else if (n == 0) {
                    // Zamknięcie wejścia (Ctrl+D)
                    do_work = 0;
                }
            } 
            else if (events[i].data.fd == sock) {
                // Odczyt wiadomości od serwera biblioteki
                char msg[MESSAGE_SIZE + 1];
                ssize_t b = bulk_read(sock, msg, MESSAGE_SIZE);
                
                if (b == MESSAGE_SIZE) {
                    msg[MESSAGE_SIZE] = '\0';
                    
                    // Weryfikacja poprawności formatu i aktualizacja stanu
                    if ((msg[0] == 'p' || msg[0] == 'g') && msg[3] == '\n') {
                        int city = (msg[1] - '0') * 10 + (msg[2] - '0');
                        
                        if (city >= 1 && city <= NUM_CITIES) {
                            cities[city] = msg[0];
                            printf("\n[Biblioteka] Przechwycono nowe wieści! Miasto %02d zostało zajęte przez %s.\n", 
                                   city, msg[0] == 'g' ? "Greków" : "Persów");
                        }
                    }
                } else if (b <= 0) {
                    // Rozłączenie z serwerem
                    printf("\nPołączenie z serwerem zostało zerwane.\n");
                    do_work = 0;
                }
            }
        }
    }

    // Wykonanie obowiązków po otrzymaniu SIGINT / wyjściu
    printf("\n\nOpuszczanie sieci posłańców. Generowanie raportu końcowego...");
    print_owners(cities);

    // Zwalnianie zasobów
    if (close(sock) < 0) perror("Błąd podczas zamykania gniazda");
    if (close(epoll_fd) < 0) perror("Błąd podczas zamykania epoll");

    printf("Zasoby zwolnione. Pomyślne zakończenie działania programu.\n");
    return EXIT_SUCCESS;
}