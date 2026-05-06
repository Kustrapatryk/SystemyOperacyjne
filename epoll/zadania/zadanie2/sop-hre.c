#include "l4-common.h"
#include <pthread.h>

#define BACKLOG_SIZE 10
#define MAX_EVENTS 16
#define BUF_SIZE 256
#define MAX_FDS 1024

// --- ZMIENNE GLOBALNE I SYNCHRONIZACJA ---
volatile sig_atomic_t do_work = 1; // Flaga sterująca życiem serwera

const char *elector_names[] = {
    "", "Moguncja", "Trewir", "Kolonia", "Czechy", "Palatynat", "Saksonia", "Brandenburgia"
};

int elector_votes[8];
pthread_mutex_t votes_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- OBSŁUGA SYGNAŁÓW ---
void sigint_handler(int sig) {
    (void)sig;
    do_work = 0; // Sygnał SIGINT (Ctrl+C) przełącza flagę
}

void usage(char* name) {
    fprintf(stderr, "USAGE: %s tcp_port udp_port\n", name);
    exit(EXIT_FAILURE);
}

// --- WĄTEK UDP ---
void *udp_broadcaster(void *arg) {
    char *udp_port_str = (char *)arg;
    int udp_socket = socket(PF_INET, SOCK_DGRAM, 0);
    if (udp_socket < 0) ERR("socket UDP");

    struct sockaddr_in dest_addr = make_address("127.0.0.1", udp_port_str);

    // Wątek działa dopóki nie nadejdzie SIGINT
    while (do_work) {
        // Obliczamy wyniki
        pthread_mutex_lock(&votes_mutex);
        int f1 = 0, f2 = 0, f3 = 0;
        for (int i = 1; i <= 7; i++) {
            if (elector_votes[i] == 1) f1++;
            else if (elector_votes[i] == 2) f2++;
            else if (elector_votes[i] == 3) f3++;
        }
        pthread_mutex_unlock(&votes_mutex);

        char msg[128];
        snprintf(msg, sizeof(msg), "Wyniki - Franciszek I: %d, Karol V: %d, Henryk VIII: %d\n", f1, f2, f3);
        
        if (sendto(udp_socket, msg, strlen(msg), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
            perror("sendto UDP");
        }
        
        sleep(1); // Odczekaj sekundę (jeśli przyjdzie SIGINT, sleep zostanie przerwany)
    }
    
    TEMP_FAILURE_RETRY(close(udp_socket));
    return NULL;
}

int main(int argc, char **argv) { 
    if (argc != 3) {
        usage(argv[0]);
    }

    // Ustawienie handlera dla SIGINT
    if (sethandler(sigint_handler, SIGINT)) ERR("sethandler");

    int client_to_elector[MAX_FDS]; 
    int elector_fd[8];

    // Inicjalizacja stanu: -2 oznacza całkowicie wolne gniazdo
    for (int i = 0; i < MAX_FDS; i++) client_to_elector[i] = -2; 
    for (int i = 0; i < 8; i++) {
        elector_fd[i] = -1;
        elector_votes[i] = 0;
    }

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

    printf("Serwer TCP (port: %d), UDP (port: %s) uruchomiony. Wcisnij Ctrl+C, aby zakonczyc.\n", tcp_port, argv[2]);

    // Główna pętla przerywana przez SIGINT
    while (do_work) {
        int nfds = epoll_wait(epoll_descriptor, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            // Jeśli epoll_wait został przerwany przez nasz SIGINT, przejdziemy do kolejnej iteracji, 
            // gdzie while(do_work) zwróci fałsz i opuścimy pętlę.
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

                client_to_elector[client_socket] = -1; // -1 = połączony, czeka na identyfikację
                event.events = EPOLLIN;
                event.data.fd = client_socket;
                epoll_ctl(epoll_descriptor, EPOLL_CTL_ADD, client_socket, &event);

            } else {
                int client_socket = events[n].data.fd;
                char buf[BUF_SIZE];
                ssize_t size = TEMP_FAILURE_RETRY(read(client_socket, buf, BUF_SIZE - 1));
                
                if (size <= 0) {
                    int e_id = client_to_elector[client_socket];
                    if (e_id > 0) elector_fd[e_id] = -1; 
                    
                    client_to_elector[client_socket] = -2; // Oznaczamy deskryptor jako wolny
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
                        client_to_elector[client_socket] = -2;
                        epoll_ctl(epoll_descriptor, EPOLL_CTL_DEL, client_socket, NULL);
                        TEMP_FAILURE_RETRY(close(client_socket));
                        continue;
                    }

                    if (auth_id != -1) {
                        if (elector_fd[auth_id] != -1) {
                            char *msg = "This elector is already connected!\n";
                            bulk_write(client_socket, msg, strlen(msg));
                            client_to_elector[client_socket] = -2;
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
                            
                            pthread_mutex_lock(&votes_mutex);
                            elector_votes[e_id] = kandydat;
                            pthread_mutex_unlock(&votes_mutex);
                            
                            char *ack = "Vote registered!\n";
                            bulk_write(client_socket, ack, strlen(ack));
                        }
                    }
                }
            }
        }
    }

    // ==========================================
    // ETAP 5: FAZA SPRZĄTANIA PO OTRZYMANIU SIGINT
    // ==========================================
    printf("\nZatrzymywanie serwera. Czekam na watek UDP...\n");
    
    // 1. Zakończenie wątku UDP
    pthread_join(udp_thread, NULL); 

    // 2. Zamknięcie wszystkich otwartych połączeń klientów
    for (int i = 0; i < MAX_FDS; i++) {
        if (client_to_elector[i] != -2) {
            epoll_ctl(epoll_descriptor, EPOLL_CTL_DEL, i, NULL);
            TEMP_FAILURE_RETRY(close(i));
        }
    }

    // 3. Zamknięcie głównych zasobów
    TEMP_FAILURE_RETRY(close(epoll_descriptor));
    TEMP_FAILURE_RETRY(close(listen_socket));

    // 4. Obliczenie i wypisanie ostatecznych wyników (bez muteksu, bo wątki już nie działają)
    int final_1 = 0, final_2 = 0, final_3 = 0;
    for (int i = 1; i <= 7; i++) {
        if (elector_votes[i] == 1) final_1++;
        else if (elector_votes[i] == 2) final_2++;
        else if (elector_votes[i] == 3) final_3++;
    }

    printf("\n*** OSTATECZNE WYNIKI WYBOROW CESARSKICH (1519) ***\n");
    printf("1. Franciszek I: %d glosow\n", final_1);
    printf("2. Karol V:      %d glosow\n", final_2);
    printf("3. Henryk VIII:  %d glosow\n", final_3);
    printf("***************************************************\n");

    return EXIT_SUCCESS;
}