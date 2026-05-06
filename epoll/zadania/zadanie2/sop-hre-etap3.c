#include "l4-common.h"

#define BACKLOG_SIZE 10
#define MAX_EVENTS 16
#define BUF_SIZE 256
#define MAX_FDS 1024

// Nazwy elektorów (indeks 0 zostawiamy pusty dla wygody, używamy 1-7)
const char *elector_names[] = {
    "", 
    "Moguncja", 
    "Trewir", 
    "Kolonia", 
    "Czechy", 
    "Palatynat", 
    "Saksonia", 
    "Brandenburgia"
};

void usage(char* name) {
    fprintf(stderr, "USAGE: %s port\n", name);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv) { 
    if (argc != 2) {
        usage(argv[0]);
    }

    // --- STAN SERWERA ---
    // client_to_elector[fd] = id_elektora. Jeśli -1, to klient jeszcze nie zidentyfikowany.
    int client_to_elector[MAX_FDS]; 
    // elector_fd[id_elektora] = fd. Jeśli -1, to elektor nie jest aktualnie połączony.
    int elector_fd[8];
    // elector_votes[id_elektora] = numer_kandydata (0 oznacza brak głosu)
    int elector_votes[8];

    // Inicjalizacja stanu
    for (int i = 0; i < MAX_FDS; i++) client_to_elector[i] = -1;
    for (int i = 0; i < 8; i++) {
        elector_fd[i] = -1;
        elector_votes[i] = 0;
    }

    uint16_t port = (uint16_t)atoi(argv[1]);
    int listen_socket = bind_tcp_socket(port, BACKLOG_SIZE);

    int epoll_descriptor;
    if ((epoll_descriptor = epoll_create1(0)) < 0) ERR("epoll_create1");

    struct epoll_event event, events[MAX_EVENTS];
    event.events = EPOLLIN;
    event.data.fd = listen_socket;
    if (epoll_ctl(epoll_descriptor, EPOLL_CTL_ADD, listen_socket, &event) == -1) ERR("epoll_ctl: listen_socket");

    printf("Serwer wyborczy uruchomiony na porcie %d...\n", port);

    while (1) {
        int nfds = epoll_wait(epoll_descriptor, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            ERR("epoll_wait");
        }

        for (int n = 0; n < nfds; n++) {
            if (events[n].data.fd == listen_socket) {
                // NOWE POŁĄCZENIE
                int client_socket = add_new_client(listen_socket);
                if (client_socket == -1) continue;
                if (client_socket >= MAX_FDS) {
                    close(client_socket); // Zabezpieczenie przed przepełnieniem tablicy
                    continue; 
                }

                printf("-> Nowe polaczenie (fd: %d). Czekam na identyfikacje...\n", client_socket);
                client_to_elector[client_socket] = -1; // Oznaczamy jako niezidentyfikowany

                event.events = EPOLLIN;
                event.data.fd = client_socket;
                epoll_ctl(epoll_descriptor, EPOLL_CTL_ADD, client_socket, &event);

            } else {
                // DANE OD KLIENTA
                int client_socket = events[n].data.fd;
                char buf[BUF_SIZE];
                ssize_t size = TEMP_FAILURE_RETRY(read(client_socket, buf, BUF_SIZE - 1));
                
                if (size <= 0) {
                    // ZERWANIE POŁĄCZENIA LUB BŁĄD
                    int e_id = client_to_elector[client_socket];
                    if (e_id != -1) {
                        printf("<- Elektor %s rozlaczyl sie.\n", elector_names[e_id]);
                        elector_fd[e_id] = -1; // Zwalniamy miejsce dla tego elektora
                    } else {
                        printf("<- Niezidentyfikowany klient rozlaczyl sie.\n");
                    }
                    
                    client_to_elector[client_socket] = -1;
                    epoll_ctl(epoll_descriptor, EPOLL_CTL_DEL, client_socket, NULL);
                    TEMP_FAILURE_RETRY(close(client_socket));
                    continue;
                }

                buf[size] = '\0';
                int e_id = client_to_elector[client_socket];

                // FAZA 1: IDENTYFIKACJA
                if (e_id == -1) {
                    int auth_id = -1;
                    int invalid_char_found = 0;

                    // Szukamy pierwszej cyfry, ignorując białe znaki (np. \n wysyłane przez netcat)
                    for (int i = 0; i < size; i++) {
                        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' ') continue;
                        
                        if (buf[i] >= '1' && buf[i] <= '7') {
                            auth_id = buf[i] - '0';
                            break; // Znaleziono prawidłowe ID
                        } else {
                            invalid_char_found = 1; // Znaleziono niedozwolony znak
                            break;
                        }
                    }

                    if (invalid_char_found) {
                        printf("Odrzucono fd %d: Bledny znak identyfikacyjny.\n", client_socket);
                        epoll_ctl(epoll_descriptor, EPOLL_CTL_DEL, client_socket, NULL);
                        TEMP_FAILURE_RETRY(close(client_socket));
                        continue;
                    }

                    if (auth_id != -1) {
                        // Sprawdzamy, czy elektor nie jest już zalogowany
                        if (elector_fd[auth_id] != -1) {
                            char *msg = "This elector is already connected!\n";
                            bulk_write(client_socket, msg, strlen(msg));
                            printf("Odrzucono fd %d: Proba podwojnego logowania na elektora %s.\n", client_socket, elector_names[auth_id]);
                            epoll_ctl(epoll_descriptor, EPOLL_CTL_DEL, client_socket, NULL);
                            TEMP_FAILURE_RETRY(close(client_socket));
                        } else {
                            // Sukces identyfikacji
                            client_to_elector[client_socket] = auth_id;
                            elector_fd[auth_id] = client_socket;
                            
                            char welcome_msg[256];
                            snprintf(welcome_msg, sizeof(welcome_msg), "Welcome, elector of %s!\n", elector_names[auth_id]);
                            bulk_write(client_socket, welcome_msg, strlen(welcome_msg));
                            printf("Zalogowano elektora: %s (fd: %d)\n", elector_names[auth_id], client_socket);
                        }
                    }
                } 
                // FAZA 2: GŁOSOWANIE
                else {
                    for (int i = 0; i < size; i++) {
                        if (buf[i] >= '1' && buf[i] <= '3') {
                            elector_votes[e_id] = buf[i] - '0';
                            printf("*** Elektor %s oddal/zmienil glos na kandydata nr %d ***\n", elector_names[e_id], elector_votes[e_id]);
                            
                            char *ack = "Vote registered!\n";
                            bulk_write(client_socket, ack, strlen(ack));
                        }
                    }
                }
            }
        }
    }

    close(epoll_descriptor);
    close(listen_socket);
    return EXIT_SUCCESS;
}