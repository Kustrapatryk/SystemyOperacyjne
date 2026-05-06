#include "l7_common.h"
#include <ctype.h>

#define BACKLOG 5
#define MAX_EVENTS 16
#define BUF_SIZE 16

volatile sig_atomic_t do_work = 1;

// Handler sygnału SIGINT (Ctrl+C)
void sigint_handler(int sig) { 
    do_work = 0; 
}

void usage(char *name) { 
    fprintf(stderr, "USAGE: %s port\n", name); 
}

int main(int argc, char **argv) {
    if (argc != 2) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    // Bezpieczeństwo - ignorujemy SIGPIPE, by uszkodzone połączenie nie zabiło serwera
    if (sethandler(SIG_IGN, SIGPIPE)) ERR("Setting SIGPIPE:");
    if (sethandler(sigint_handler, SIGINT)) ERR("Setting SIGINT:");

    // Inicjalizacja gniazda TCP
    int listen_socket = bind_tcp_socket(atoi(argv[1]), BACKLOG);
    int new_flags = fcntl(listen_socket, F_GETFL) | O_NONBLOCK;
    fcntl(listen_socket, F_SETFL, new_flags);

    // Inicjalizacja epoll
    int epoll_descriptor;
    if ((epoll_descriptor = epoll_create1(0)) < 0) ERR("epoll_create:");

    struct epoll_event event, events[MAX_EVENTS];
    event.events = EPOLLIN;
    event.data.fd = listen_socket;
    if (epoll_ctl(epoll_descriptor, EPOLL_CTL_ADD, listen_socket, &event) == -1)
        ERR("epoll_ctl: listen_sock");

    int nfds;
    int16_t highest_sum = -1; // -1 oznacza, że jeszcze nic nie otrzymaliśmy

    // Maskowanie sygnału SIGINT na czas działania innych funkcji niż pwait
    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, &oldmask);

    printf("Serwer TCP uruchomiony na porcie %s. Oczekuję na klientów...\n", argv[1]);

    while (do_work) {
        // epoll_pwait "odblokowuje" SIGINT na ułamek sekundy, w którym czeka na zdarzenie
        if ((nfds = epoll_pwait(epoll_descriptor, events, MAX_EVENTS, -1, &oldmask)) > 0) {
            for (int n = 0; n < nfds; n++) {
                if (events[n].data.fd == listen_socket) {
                    // Akceptacja nowego klienta
                    int client_socket = add_new_client(listen_socket);
                    if (client_socket == -1) continue; // EAGAIN - brak realnych połączeń w kolejce

                    char buf[BUF_SIZE];
                    memset(buf, 0, BUF_SIZE);

                    // Odczyt PIDu od klienta jako tekst (do 16 znaków)
                    ssize_t size = bulk_read(client_socket, buf, BUF_SIZE);
                    
                    if (size < 0) {
                        perror("read");
                    } else if (size > 0) {
                        printf("-> Podłączył się klient, otrzymany tekst: %s\n", buf);

                        // Liczenie sumy cyfr
                        int16_t sum = 0;
                        for (int i = 0; i < BUF_SIZE && buf[i] != '\0'; i++) {
                            if (isdigit(buf[i])) {
                                sum += buf[i] - '0';
                            }
                        }

                        // Aktualizacja najwyższego wyniku
                        if (sum > highest_sum) {
                            highest_sum = sum;
                        }

                        // Odsyłanie wyniku do klienta
                        // Zamieniamy na standard sieciowy Big-Endian (htons - Host TO Network Short)
                        int16_t net_sum = htons(sum);
                        if (bulk_write(client_socket, (char *)&net_sum, sizeof(int16_t)) < 0 && errno != EPIPE) {
                            perror("write");
                        }
                    }

                    // Zamknięcie połączenia po wymianie danych
                    if (TEMP_FAILURE_RETRY(close(client_socket)) < 0) {
                        perror("close client socket");
                    }
                }
            }
        } else {
            // Jeśli pwait przerwał SIGINT, wracamy na początek pętli, gdzie do_work będzie 0
            if (errno == EINTR) continue;
            ERR("epoll_pwait");
        }
    }

    // --- SPRZĄTANIE PO OTRZYMANIU SIGINT ---
    if (TEMP_FAILURE_RETRY(close(epoll_descriptor)) < 0) ERR("close epoll");
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
    if (TEMP_FAILURE_RETRY(close(listen_socket)) < 0) ERR("close listen_socket");

    printf("\nZatrzymano serwer (SIGINT).\n");
    if (highest_sum == -1) {
        printf("Nie obsłużono żadnego klienta.\n");
    } else {
        printf("Najwyższa otrzymana suma cyfr to: %d\n", highest_sum);
    }

    return EXIT_SUCCESS;
}