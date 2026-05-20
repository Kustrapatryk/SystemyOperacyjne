#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define MAX_BUFFER 256
#define MAX_NAME_LEN 128

int main(int argc, char *argv[]) {
    // 1. Sprawdzenie argumentów
    if (argc != 2) {
        fprintf(stderr, "Uzycie: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = atoi(argv[1]);

    // 2. Utworzenie gniazda datagramowego (UDP)
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    // 3. Konfiguracja adresu serwera
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Nasłuchuj na wszystkich interfejsach
    server_addr.sin_port = htons(port);       // Port podany przez użytkownika

    // Zabezpieczenie przed błędem "Address already in use"
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 4. Przypisanie gniazda do portu (Bind)
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Sztab Napoleona nasluchuje meldunkow na porcie %d...\n", port);
    printf("Gotowy do odbioru. Nacisnij Ctrl+C aby zakonczyc.\n\n");

    char buffer[MAX_BUFFER];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    // 5. Główna pętla nasłuchująca
    while (1) {
        // Odbieranie datagramu
        ssize_t n = recvfrom(sockfd, buffer, MAX_BUFFER - 1, 0, 
                             (struct sockaddr *)&client_addr, &client_len);
                             
        if (n < 0) {
            perror("Błąd recvfrom");
            continue; // Ważne: w razie błędu nie kończymy programu! Czekamy na kolejny meldunek.
        }

        // Zabezpieczenie bufora znakiem końca stringa
        buffer[n] = '\0';

        // Zmienne do sparsowania
        int x, y, p;
        char name[MAX_NAME_LEN + 1];

        // 6. Parsowanie wiadomości przy użyciu sscanf
        // Format: "%d %d %d %128[^\n]"
        // %d - wczytuje liczbe calkowita
        // %128[^\n] - wczytuje maksymalnie 128 znakow (w tym spacje) az do napotkania znaku nowej linii
        int parsed_items = sscanf(buffer, "%d %d %d %128[^\n]", &x, &y, &p, name);

        // 7. Walidacja
        if (parsed_items != 4) {
            fprintf(stderr, "[BŁĄD] Zły format meldunku! Odrzucono: %s\n", buffer);
            continue;
        }

        if (x < 0 || x > 99 || y < 0 || y > 99) {
            fprintf(stderr, "[BŁĄD] Nieprawidlowe wspolrzedne (X:%d, Y:%d). Wymagany zakres to 0-99.\n", x, y);
            continue;
        }

        if (p != 0 && p != 1) {
            fprintf(stderr, "[BŁĄD] Nieznana przynaleznosc (P:%d). Wymagane 0 (wrogi) lub 1 (sojuszniczy).\n", p);
            continue;
        }

        // 8. Sukces - Wypisanie sformatowanego komunikatu
        const char *affiliation = (p == 1) ? "Nasz" : "Wrogi";
        printf("%s oddzial %s byl widziany na pozycji %d:%d\n", affiliation, name, x, y);
    }

    // Kod nigdy tu nie dotrze z powodu while(1), chyba że dodalibyśmy obsługę SIGINT.
    close(sockfd);
    return EXIT_SUCCESS;
}