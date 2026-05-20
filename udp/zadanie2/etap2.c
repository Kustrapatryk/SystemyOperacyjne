#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <semaphore.h>

#define MAX_BUFFER 256
#define MAX_NAME_LEN 128
#define STACK_SIZE 16
#define NUM_ADJUTANTS 4

// Struktura przechowująca pojedynczy meldunek
typedef struct {
    char text[MAX_BUFFER];
    struct sockaddr_in client_addr; // Przechowujemy adres nadawcy (przyda się w Etapie 4)
} Message;

// Globalna pamięć współdzielona (Stos)
Message message_stack[STACK_SIZE];
int stack_top = 0; // Wskazuje na pierwsze wolne miejsce na stosie

// Narzędzia do synchronizacji
pthread_mutex_t stack_mutex = PTHREAD_MUTEX_INITIALIZER; // Chroni dostęp do tablicy message_stack i zmiennej stack_top
sem_t sem_items;  // Zlicza, ile wiadomości leży na stosie
sem_t sem_spaces; // Zlicza, ile jest jeszcze wolnych miejsc na stosie

// ==========================================
// KOD ADIUTANTA (Wątek Konsumenta)
// ==========================================
void* adjutant_worker(void* arg) {
    int id = *((int*)arg);
    free(arg); // Zwalniamy pamięć zaalokowaną dla ID wątku

    printf("[Adiutant %d] Zglasza gotowosc w sztabie!\n", id);

    while (1) {
        // 1. Czekaj, aż na stosie pojawi się jakaś praca (wiadomość)
        sem_wait(&sem_items);

        // 2. Zablokuj dostęp do stosu, aby inni adiutanci nie wzięli tego samego meldunku
        pthread_mutex_lock(&stack_mutex);

        // 3. Zdejmij wiadomość ze stosu (LIFO - Last In, First Out)
        stack_top--;
        Message msg = message_stack[stack_top];

        // 4. Odblokuj stos dla innych wątków
        pthread_mutex_unlock(&stack_mutex);

        // 5. Zasygnalizuj głównemu wątkowi, że na stosie zwolniło się jedno miejsce
        sem_post(&sem_spaces);

        // 6. Parsowanie i walidacja (Logika przeniesiona z Etapu 1)
        int x, y, p;
        char name[MAX_NAME_LEN + 1];

        int parsed_items = sscanf(msg.text, "%d %d %d %128[^\n]", &x, &y, &p, name);

        if (parsed_items != 4) {
            fprintf(stderr, "[Adiutant %d] [BŁĄD] Zły format: %s\n", id, msg.text);
            continue;
        }

        if (x < 0 || x > 99 || y < 0 || y > 99) {
            fprintf(stderr, "[Adiutant %d] [BŁĄD] Złe wspolrzedne (X:%d, Y:%d)\n", id, x, y);
            continue;
        }

        if (p != 0 && p != 1) {
            fprintf(stderr, "[Adiutant %d] [BŁĄD] Zla przynaleznosc (P:%d)\n", id, p);
            continue;
        }

        // 7. Wypisanie informacji
        const char *affiliation = (p == 1) ? "Nasz" : "Wrogi";
        printf("[Adiutant %d] %s oddzial %s byl widziany na pozycji %d:%d\n", 
               id, affiliation, name, x, y);
    }
    return NULL;
}

// ==========================================
// GŁÓWNY WĄTEK SERWERA (Wątek Producenta)
// ==========================================
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uzycie: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = atoi(argv[1]);

    // 1. Inicjalizacja semaforów
    // sem_spaces: Początkowo cały stos jest pusty, więc mamy STACK_SIZE (16) wolnych miejsc.
    sem_init(&sem_spaces, 0, STACK_SIZE);
    // sem_items: Początkowo nie ma żadnych wiadomości, więc wartość to 0.
    sem_init(&sem_items, 0, 0);

    // 2. Uruchomienie puli wątków (4 adiutantów)
    pthread_t adjutants[NUM_ADJUTANTS];
    for (int i = 0; i < NUM_ADJUTANTS; i++) {
        int *id = malloc(sizeof(int));
        *id = i + 1;
        if (pthread_create(&adjutants[i], NULL, adjutant_worker, id) != 0) {
            perror("Błąd tworzenia wątku");
            return EXIT_FAILURE;
        }
    }

    // 3. Konfiguracja gniazda UDP
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        return EXIT_FAILURE;
    }

    printf("\nSztab Napoleona aktywny na porcie %d. Oczekuje na poslancow...\n", port);

    // 4. Główna pętla nasłuchująca (Wątek pobiera datagramy i kładzie na stos)
    while (1) {
        Message new_msg;
        socklen_t client_len = sizeof(new_msg.client_addr);

        // Odbieramy datagram
        ssize_t n = recvfrom(sockfd, new_msg.text, MAX_BUFFER - 1, 0, 
                             (struct sockaddr *)&new_msg.client_addr, &client_len);
                             
        if (n < 0) {
            perror("Błąd recvfrom");
            continue;
        }
        new_msg.text[n] = '\0';

        // --- SEKCJA KRYTYCZNA PRODUCENTA ---

        // A. Czekamy, aż na stosie będzie co najmniej jedno wolne miejsce.
        // Jeśli napłynie naraz 17 wiadomości, wątek zablokuje się tutaj, dopóki adiutant czegoś nie zdejmie.
        sem_wait(&sem_spaces);

        // B. Blokujemy mutex stosu, aby bezpiecznie dodać wiadomość.
        pthread_mutex_lock(&stack_mutex);

        // C. Odłożenie wiadomości na samą górę stosu.
        message_stack[stack_top] = new_msg;
        stack_top++;

        // D. Odblokowujemy mutex stosu.
        pthread_mutex_unlock(&stack_mutex);

        // E. Sygnalizujemy (podnosimy semafor), że na stosie przybyła nowa wiadomość.
        // To natychmiast "obudzi" jednego z uśpionych adiutantów.
        sem_post(&sem_items);
    }

    // Poniższy kod czyszczący nie zostanie naturalnie osiągnięty przez pętlę while(1)
    close(sockfd);
    sem_destroy(&sem_items);
    sem_destroy(&sem_spaces);
    pthread_mutex_destroy(&stack_mutex);
    return EXIT_SUCCESS;
}