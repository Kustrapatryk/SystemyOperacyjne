#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#define MAXBUF 256
#define NUM_ELECTORS 7
#define NUM_CANDIDATES 3

// Makro do prostej obsługi błędów
#define ERR(source) (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), exit(EXIT_FAILURE))

// Makro z POSIX zabezpieczające przed przerwaniem funkcji blokujących przez sygnały
#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(expression)             \
    (__extension__({                               \
        long int __result;                         \
        do                                         \
            __result = (long int)(expression);     \
        while (__result == -1L && errno == EINTR); \
        __result;                                  \
    }))
#endif

// Zmienna globalna do obsługi bezpiecznego zakończenia pętli po SIGINT
volatile sig_atomic_t do_work = 1;

// Tablica przechowująca aktualne głosy. 
// Indeks 0 odpowiada elektorowi 1, indeks 6 elektorowi 7.
// Wartość 0 oznacza brak głosu. Wartości 1-3 to konkretni kandydaci.
int votes[NUM_ELECTORS] = {0}; 

// Nazwy do ładnego wypisywania wyników
const char* elector_names[] = {"Moguncja", "Trewir", "Kolonia", "Czechy", "Palatynat", "Saksonia", "Brandenburgia"};
const char* candidate_names[] = {"Franciszek I", "Karol V", "Henryk VIII"};


// Handler sygnału SIGINT
void sigint_handler(int sig) {
    do_work = 0; // Przełącz flagę, by wyjść z pętli serwera
}

// Funkcja konfigurująca obsługę sygnału
int sethandler(void (*f)(int), int sigNo) {
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    act.sa_handler = f;
    if (-1 == sigaction(sigNo, &act, NULL))
        return -1;
    return 0;
}

// Funkcja tworząca i bindująca gniazdo UDP
int bind_udp_socket(uint16_t port) {
    int socketfd;
    struct sockaddr_in addr;

    // Tworzenie gniazda AF_INET (IPv4), SOCK_DGRAM (UDP)
    socketfd = socket(PF_INET, SOCK_DGRAM, 0);
    if (socketfd < 0) ERR("socket");

    // Konfiguracja adresu
    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // Nasłuchuj na wszystkich interfejsach

    // Zabezpieczenie przed "Address already in use" przy restarcie serwera
    int t = 1;
    if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &t, sizeof(t)))
        ERR("setsockopt");

    // Przypisanie portu do gniazda
    if (bind(socketfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        ERR("bind");

    return socketfd;
}

// Główna logika serwera
void doServer(int fd) {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len;
    char buf[MAXBUF];
    char response[MAXBUF];
    ssize_t receivedBytes;

    printf("Serwer UDP oczekuje na glosy...\n");

    // Pętla działa dopóki nie naciśniemy Ctrl+C (do_work = 1)
    while (do_work) {
        client_addr_len = sizeof(client_addr);
        
        // Odbieranie datagramu. Używamy recvfrom, aby wiedzieć komu odesłać potwierdzenie.
        // Nie używamy tu TEMP_FAILURE_RETRY dla EINTR, ponieważ chcemy, aby sygnał SIGINT 
        // naturalnie przerwał funkcję recvfrom i pozwolił wyjść z pętli.
        receivedBytes = recvfrom(fd, buf, MAXBUF - 1, 0, (struct sockaddr *)&client_addr, &client_addr_len);
        
        if (receivedBytes < 0) {
            if (errno == EINTR) {
                // recvfrom zostało przerwane przez SIGINT. Flaga do_work zmieniła się na 0, pętla się zaraz skończy.
                continue; 
            }
            ERR("recvfrom");
        }

        buf[receivedBytes] = '\0'; // Bezpieczne zakończenie bufora (c-string)

        // Sprawdzamy czy pakiet ma sens (minimum 2 znaki)
        if (receivedBytes < 2) {
            snprintf(response, MAXBUF, "ERR: Zbyt krotka wiadomosc (format: XY)\n");
            TEMP_FAILURE_RETRY(sendto(fd, response, strlen(response), 0, (struct sockaddr *)&client_addr, client_addr_len));
            continue;
        }

        // Dekodowanie znaków ASCII na liczby. 
        // np. jeśli klient wpisał "12", to buf[0] == '1' (kod 49), buf[1] == '2' (kod 50)
        int elector_id = buf[0] - '0';
        int candidate_id = buf[1] - '0';

        // Walidacja elektora (1-7)
        if (elector_id < 1 || elector_id > NUM_ELECTORS) {
            snprintf(response, MAXBUF, "ERR: Nieznany elektor! Dozwolone (1-7).\n");
            TEMP_FAILURE_RETRY(sendto(fd, response, strlen(response), 0, (struct sockaddr *)&client_addr, client_addr_len));
            continue;
        }

        // Walidacja kandydata (1-3)
        if (candidate_id < 1 || candidate_id > NUM_CANDIDATES) {
            snprintf(response, MAXBUF, "ERR: Nieznany kandydat! Dozwolone (1-3).\n");
            TEMP_FAILURE_RETRY(sendto(fd, response, strlen(response), 0, (struct sockaddr *)&client_addr, client_addr_len));
            continue;
        }

        // Rejestracja głosu (indeksy w tablicach w C są od 0, więc odejmujemy 1)
        votes[elector_id - 1] = candidate_id;

        // Odesłanie potwierdzenia do klienta
        snprintf(response, MAXBUF, "OK: %s oddal glos na kandydata %s.\n", 
                 elector_names[elector_id - 1], candidate_names[candidate_id - 1]);
                 
        if (TEMP_FAILURE_RETRY(sendto(fd, response, strlen(response), 0, (struct sockaddr *)&client_addr, client_addr_len)) < 0) {
            ERR("sendto");
        }
        
        printf("[Log]: Odebrano glos: %s -> %s\n", elector_names[elector_id - 1], candidate_names[candidate_id - 1]);
    }
}

// Funkcja drukująca wyniki po zakończeniu pracy serwera
void print_results() {
    int candidate_scores[NUM_CANDIDATES] = {0};
    int missing_votes = 0;

    printf("\n\n--- WYNIKI WYBOROW CESARSKICH ---\n");
    
    // Zliczanie głosów
    for (int i = 0; i < NUM_ELECTORS; i++) {
        if (votes[i] == 0) {
            missing_votes++;
            printf("Elektor %s: WSTRZYMAL SIE\n", elector_names[i]);
        } else {
            printf("Elektor %s: Zaglosowal na %s\n", elector_names[i], candidate_names[votes[i] - 1]);
            candidate_scores[votes[i] - 1]++;
        }
    }

    printf("\nPodsumowanie glosow:\n");
    for(int i = 0; i < NUM_CANDIDATES; i++) {
        printf(" [%d] %s: %d glosow\n", i+1, candidate_names[i], candidate_scores[i]);
    }
    
    printf("Brakujace glosy: %d\n", missing_votes);
    printf("---------------------------------\n");
}


int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Uzycie: %s port\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Podpięcie handlera dla sygnału SIGINT (Crtl+C)
    if (sethandler(sigint_handler, SIGINT))
        ERR("sethandler SIGINT");

    // Konfiguracja gniazda
    int port = atoi(argv[1]);
    int fd = bind_udp_socket(port);

    // Główna pętla programu
    doServer(fd);

    // Kiedy otrzymamy SIGINT, funkcja doServer() się zakończy i program przejdzie tutaj.
    print_results();

    // Sprzątanie zasobów
    if (TEMP_FAILURE_RETRY(close(fd)) < 0)
        ERR("close");

    printf("Serwer zakonczyl dzialanie pomsylnie.\n");
    return EXIT_SUCCESS;
}