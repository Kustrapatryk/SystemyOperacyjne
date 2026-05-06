#include "l7_common.h"

#define BUF_SIZE 16

void usage(char *name) { 
    fprintf(stderr, "USAGE: %s address port\n", name); 
}

int main(int argc, char **argv) {
    if (argc != 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    // Pobranie i wypisanie własnego PIDu
    pid_t my_pid = getpid();
    printf("Uruchomiono klienta. Mój PID to: %d\n", my_pid);

    // Połączenie z serwerem
    int fd = connect_tcp_socket(argv[1], argv[2]);

    // Przygotowanie paczki danych (konwersja PID na tekst)
    char buf[BUF_SIZE];
    memset(buf, 0, BUF_SIZE); // Wypełnienie zerami, aby "puste" miejsce po krótszym PIDzie było puste
    snprintf(buf, BUF_SIZE, "%d", my_pid);

    // Wysyłanie PIDu do serwera
    if (bulk_write(fd, buf, BUF_SIZE) < 0) {
        ERR("write:");
    }

    // Oczekiwanie i odbieranie wyliczonego wyniku
    int16_t result;
    if (bulk_read(fd, (char *)&result, sizeof(int16_t)) < (int)sizeof(int16_t)) {
        ERR("read:");
    }

    // Konwersja odebranych danych z formatu sieciowego na lokalny
    result = ntohs(result);
    printf("<- Serwer wyliczył sumę cyfr mojego PIDu na: %d\n", result);

    // Bezpieczne zamknięcie połączenia i zakończenie programu
    if (TEMP_FAILURE_RETRY(close(fd)) < 0) {
        ERR("close");
    }

    return EXIT_SUCCESS;
}