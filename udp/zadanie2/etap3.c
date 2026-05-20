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
#define MAP_SIZE 100
#define DIVISION_NAMES_SIZE 128

// --- STRUKTURY DANYCH (Etap 2) ---
typedef struct {
    char text[MAX_BUFFER];
    struct sockaddr_in client_addr;
} Message;

Message message_stack[STACK_SIZE];
int stack_top = 0;

pthread_mutex_t stack_mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t sem_items;
sem_t sem_spaces;

// --- NOWE STRUKTURY DANYCH (Etap 3) ---

// 1. Rejestr nazw oddziałów
char division_names[DIVISION_NAMES_SIZE][MAX_NAME_LEN];
int division_count = 0;
pthread_mutex_t names_mutex = PTHREAD_MUTEX_INITIALIZER; // Chroni dostęp do rejestru

// 2. Mapa sztabowa
int map[MAP_SIZE][MAP_SIZE];
pthread_mutex_t map_mutexes[MAP_SIZE]; // Tablica muteksów (jeden na każdy wiersz mapy)


// ==========================================
// KOD ADIUTANTA (Wątek Konsumenta)
// ==========================================
void* adjutant_worker(void* arg) {
    int id = *((int*)arg);
    free(arg);

    printf("[Adiutant %d] Gotowy do pracy przy mapach!\n", id);

    while (1) {
        // --- ETAP 2: Pobieranie zadania ---
        sem_wait(&sem_items);
        
        pthread_mutex_lock(&stack_mutex);
        stack_top--;
        Message msg = message_stack[stack_top];
        pthread_mutex_unlock(&stack_mutex);
        
        sem_post(&sem_spaces);

        // Parsowanie
        int x, y, p;
        char name[MAX_NAME_LEN + 1];
        int parsed_items = sscanf(msg.text, "%d %d %d %128[^\n]", &x, &y, &p, name);

        if (parsed_items != 4 || x < 0 || x >= MAP_SIZE || y < 0 || y >= MAP_SIZE || (p != 0 && p != 1)) {
            fprintf(stderr, "[Adiutant %d] [BŁĄD] Odrzucono nieprawidlowy meldunek: %s\n", id, msg.text);
            continue;
        }

        // --- ETAP 3: Praca nad mapą sztabową ---

        // 1. Symulacja pracy sztabowej (10ms)
        usleep(10000); 

        // 2. Rejestracja oddziału w księgach (Sekcja Krytyczna)
        int div_idx = -1;
        
        pthread_mutex_lock(&names_mutex);
        // Szukamy, czy oddział jest już znany
        for (int i = 0; i < division_count; i++) {
            if (strcmp(division_names[i], name) == 0) {
                div_idx = i;
                break;
            }
        }
        
        // Jeśli nie znamy oddziału, dodajemy go na koniec listy
        if (div_idx == -1) {
            if (division_count < DIVISION_NAMES_SIZE) {
                div_idx = division_count;
                strncpy(division_names[div_idx], name, MAX_NAME_LEN);
                division_count++;
            } else {
                fprintf(stderr, "[Adiutant %d] [BŁĄD] Brak miejsca w rejestrze na nowy oddzial: %s\n", id, name);
                pthread_mutex_unlock(&names_mutex);
                continue; // Przerywamy obróbkę tej wiadomości, brak pamięci sztabowej
            }
        }
        pthread_mutex_unlock(&names_mutex);

        // 3. Aktualizacja pozycji na mapie

        // KROK A: Szukanie starej pozycji i wymazywanie jej (-1)
        // Zgodnie z instrukcją, szukamy na mapie, blokując wiersz po wierszu.
        int old_found = 0;
        for (int row = 0; row < MAP_SIZE; row++) {
            pthread_mutex_lock(&map_mutexes[row]);
            for (int col = 0; col < MAP_SIZE; col++) {
                if (map[row][col] == div_idx) {
                    map[row][col] = -1; // Wymazujemy starą pozycję
                    old_found = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&map_mutexes[row]);
            if (old_found) break; // Znaleźliśmy oddział, nie ma sensu szukać w kolejnych wierszach
        }

        // KROK B: Naniesienie nowej pozycji na mapę
        // Blokujemy tylko ten jeden, docelowy wiersz Y
        pthread_mutex_lock(&map_mutexes[y]);
        map[y][x] = div_idx;
        pthread_mutex_unlock(&map_mutexes[y]);

        // 4. Komunikat podsumowujący
        const char *affiliation = (p == 1) ? "Nasz" : "Wrogi";
        printf("[Adiutant %d] Zaktualizowano: %s oddzial '%s' przeniesiony na %d:%d (Indeks: %d)\n", 
               id, affiliation, name, x, y, div_idx);
    }
    return NULL;
}

// ==========================================
// GŁÓWNY WĄTEK SERWERA
// ==========================================
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uzycie: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = atoi(argv[1]);

    // --- INICJALIZACJA STRUKTUR (Etap 3) ---
    for (int i = 0; i < MAP_SIZE; i++) {
        pthread_mutex_init(&map_mutexes[i], NULL);
        for (int j = 0; j < MAP_SIZE; j++) {
            map[i][j] = -1; // -1 oznacza puste pole
        }
    }

    sem_init(&sem_spaces, 0, STACK_SIZE);
    sem_init(&sem_items, 0, 0);

    pthread_t adjutants[NUM_ADJUTANTS];
    for (int i = 0; i < NUM_ADJUTANTS; i++) {
        int *id = malloc(sizeof(int));
        *id = i + 1;
        if (pthread_create(&adjutants[i], NULL, adjutant_worker, id) != 0) {
            perror("Błąd tworzenia wątku");
            return EXIT_FAILURE;
        }
    }

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

    printf("\nSztab Napoleona aktywny na porcie %d. Adiutanci aktualizuja mapy...\n", port);

    // Główna pętla
    while (1) {
        Message new_msg;
        socklen_t client_len = sizeof(new_msg.client_addr);

        ssize_t n = recvfrom(sockfd, new_msg.text, MAX_BUFFER - 1, 0, 
                             (struct sockaddr *)&new_msg.client_addr, &client_len);
                             
        if (n < 0) {
            perror("Błąd recvfrom");
            continue;
        }
        new_msg.text[n] = '\0';

        sem_wait(&sem_spaces);
        pthread_mutex_lock(&stack_mutex);
        
        message_stack[stack_top] = new_msg;
        stack_top++;
        
        pthread_mutex_unlock(&stack_mutex);
        sem_post(&sem_items);
    }

    // Kod czyszczący
    close(sockfd);
    sem_destroy(&sem_items);
    sem_destroy(&sem_spaces);
    pthread_mutex_destroy(&stack_mutex);
    pthread_mutex_destroy(&names_mutex);
    for (int i = 0; i < MAP_SIZE; i++) {
        pthread_mutex_destroy(&map_mutexes[i]);
    }
    
    return EXIT_SUCCESS;
}