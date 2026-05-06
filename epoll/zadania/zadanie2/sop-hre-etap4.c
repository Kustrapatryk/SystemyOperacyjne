#include "l4-common.h"
#include <pthread.h> // Dodano obsługę wątków

#define BACKLOG_SIZE 10
#define MAX_EVENTS 16
#define BUF_SIZE 256
#define MAX_FDS 1024

// --- DANE GLOBALNE I SYNCHRONIZACJA ---
// Tablica z nazwami elektorów (1-7)
const char *elector_names[] = {
    "", "Moguncja", "Trewir", "Kolonia", "Czechy", "Palatynat", "Saksonia", "Brandenburgia"
};

// Wyniki głosowania (dostępne dla obu wątków)
int elector_votes[8];

// Mutex chroniący tablicę elector_votes przed wyścigiem (data race)
pthread_mutex_t votes_mutex = PTHREAD_MUTEX_INITIALIZER;

void usage(char* name) {
    fprintf(stderr, "USAGE: %s tcp_port udp_port\n", name);
    exit(EXIT_FAILURE);
}

// --- FUNKCJA NOWEGO WĄTKU (Klient UDP) ---
void *udp_broadcaster(void *arg) {
    char *udp_port_str = (char *)arg;
    
    // Tworzenie gniazda UDP (SOCK_DGRAM)
    int udp_socket = socket(PF_INET, SOCK_DGRAM, 0);
    if (udp_socket < 0) ERR("socket UDP");

    // Przygotowanie adresu docelowego (localhost i port podany w argumencie)
    struct sockaddr_in dest_addr = make_address("127.0.0.1", udp_port_str);

    while (1) {
        sleep(1); // Czekaj 1 sekundę

        // Zablokuj dostęp do wyników na czas liczenia
        pthread_mutex_lock(&votes_mutex);
        int f1 = 0, f2 = 0, f3 = 0; // Głosy na poszczególnych kandydatów (Franciszka I, Karola V, Henryka VIII)
        for (int i = 1; i <= 7; i++) {
            if (elector_votes[i] == 1) f1++;
            else if (elector_votes[i] == 2) f2++;
            else if (elector_votes[i] == 3) f3++;
        }
        pthread_mutex_unlock(&votes_mutex);

        // Formatowanie i wysyłanie wiadomości
        char msg[128];
        snprintf(msg, sizeof(msg), "Wyniki - Franciszek I: %d, Karol V: %d, Henryk VIII: %d\n", f1, f2, f3);
        
        if (sendto(udp_socket, msg, strlen(msg), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
            perror("sendto UDP");
        }
    }
    
    return NULL;
}

int main(int argc, char **argv) { 
    // Program wymaga teraz 2 argumentów: port TCP i port UDP
    if (argc != 3) {
        usage(argv[0]);
    }

    // Stan serwera TCP (lokalny dla wątku głównego)
    int client_to_elector[MAX_FDS]; 
    int elector_fd[8];

    // Inicjalizacja stanu
    for (int i = 0; i < MAX_FDS; i++) client_to_elector[i] = -1;
    for (int i = 0; i < 8; i++) {
        elector_fd[i] = -1;
        elector_votes[i] = 0; // globalna tablica
    }

    // Uruchomienie wątku UDP (przekazujemy mu port z argv[2])
    pthread_t udp_thread;
    if (pthread_create(&udp_thread, NULL, udp_broadcaster, argv[2]) != 0) {
        ERR("pthread_create");
    }

    uint16_t tcp_port = (uint16_t)atoi(argv[1]);
    int listen_socket = bind_tcp_socket(tcp_port, BACKLOG_SIZE);

    int epoll_descriptor;
    if ((epoll_descriptor = epoll_create1(0)) < 0) ERR("epoll_create1");

    struct epoll_event event, events[MAX_EVENTS];
    event.events = EPOLLIN;
    event.data.fd = listen_socket;
    if (epoll_ctl(epoll_descriptor, EPOLL_CTL_ADD, listen_socket, &event) == -1) ERR("epoll_ctl");

    printf("Serwer TCP gotowy (port: %d), nadawanie UDP w toku (port: %s)...\n", tcp_port, argv[2]);

    while (1) {
        int nfds = epoll_wait(epoll_descriptor, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            ERR("epoll_wait");
        }

        for (int n = 0; n < nfds; n++) {
            if (events[n].data.fd == listen_socket) {
                int client_socket = add_new_client(listen_socket);
                if (client_socket == -1 || client_socket >= MAX_FDS) {
                    if(client_socket >= MAX_FDS) close(client_socket);
                    continue; 
                }

                client_to_elector[client_socket] = -1;
                event.events = EPOLLIN;
                event.data.fd = client_socket;
                epoll_ctl(epoll_descriptor, EPOLL_CTL_ADD, client_socket, &event);

            } else {
                int client_socket = events[n].data.fd;
                char buf[BUF_SIZE];
                ssize_t size = TEMP_FAILURE_RETRY(read(client_socket, buf, BUF_SIZE - 1));
                
                if (size <= 0) {
                    int e_id = client_to_elector[client_socket];
                    if (e_id != -1) {
                        elector_fd[e_id] = -1; 
                        printf("<- Elektor %s rozlaczyl sie.\n", elector_names[e_id]);
                    }
                    client_to_elector[client_socket] = -1;
                    epoll_ctl(epoll_descriptor, EPOLL_CTL_DEL, client_socket, NULL);
                    TEMP_FAILURE_RETRY(close(client_socket));
                    continue;
                }

                buf[size] = '\0';
                int e_id = client_to_elector[client_socket];

                if (e_id == -1) {
                    int auth_id = -1, invalid_char = 0;
                    for (int i = 0; i < size; i++) {
                        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' ') continue;
                        if (buf[i] >= '1' && buf[i] <= '7') { auth_id = buf[i] - '0'; break; }
                        else { invalid_char = 1; break; }
                    }

                    if (invalid_char) {
                        epoll_ctl(epoll_descriptor, EPOLL_CTL_DEL, client_socket, NULL);
                        TEMP_FAILURE_RETRY(close(client_socket));
                        continue;
                    }

                    if (auth_id != -1) {
                        if (elector_fd[auth_id] != -1) {
                            char *msg = "This elector is already connected!\n";
                            bulk_write(client_socket, msg, strlen(msg));
                            epoll_ctl(epoll_descriptor, EPOLL_CTL_DEL, client_socket, NULL);
                            TEMP_FAILURE_RETRY(close(client_socket));
                        } else {
                            client_to_elector[client_socket] = auth_id;
                            elector_fd[auth_id] = client_socket;
                            char msg[256];
                            snprintf(msg, sizeof(msg), "Welcome, elector of %s!\n", elector_names[auth_id]);
                            bulk_write(client_socket, msg, strlen(msg));
                        }
                    }
                } else {
                    for (int i = 0; i < size; i++) {
                        if (buf[i] >= '1' && buf[i] <= '3') {
                            int kandydat = buf[i] - '0';
                            
                            // SEKCJA KRYTYCZNA: Zapisujemy glos uzywajac muteksu!
                            pthread_mutex_lock(&votes_mutex);
                            elector_votes[e_id] = kandydat;
                            pthread_mutex_unlock(&votes_mutex);
                            
                            printf("*** Elektor %s zaglosowal na kandydata nr %d ***\n", elector_names[e_id], kandydat);
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