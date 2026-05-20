#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#define MAX_BUFFER 256
#define MAX_NAME_LEN 128
#define STACK_SIZE 16
#define NUM_ADJUTANTS 4
#define MAP_SIZE 100
#define DIVISION_NAMES_SIZE 128

// --- STRUKTURY DANYCH ---
typedef struct {
    char text[MAX_BUFFER];
    struct sockaddr_in client_addr; // Adres nadawcy
} Message;

Message message_stack[STACK_SIZE];
int stack_top = 0;

pthread_mutex_t stack_mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t sem_items;
sem_t sem_spaces;

// 1. Rozbudowany rejestr oddziałów (Etap 3 i 4)
char division_names[DIVISION_NAMES_SIZE][MAX_NAME_LEN];
int division_affiliations[DIVISION_NAMES_SIZE];         // 0 = wrogi, 1 = sojuszniczy
struct sockaddr_in division_addrs[DIVISION_NAMES_SIZE]; // ZAPAMIĘTANY ADRES NADAWCY
int division_count = 0;
pthread_mutex_t names_mutex = PTHREAD_MUTEX_INITIALIZER; 

// 2. Mapa sztabowa
int map[MAP_SIZE][MAP_SIZE];
pthread_mutex_t map_mutexes[MAP_SIZE];

// Gniazdo serwera (globalne, by Napoleon mógł wysyłać rozkazy)
int server_sockfd;

// ==========================================
// KOD NAPOLEONA (Wątek Dowódcy)
// ==========================================
void* napoleon_worker(void* arg) {
    printf("[Napoleon] Cesarz wkroczyl do sztabu!\n");

    while (1) {
        // 1. Sen (Cesarz wydaje rozkazy co 30ms)
        usleep(30000); 

        // 2. Wypisanie stanu mapy
        // Przeglądamy mapę. Aby nie zalać terminala 10000 linii, wypisujemy tylko zajęte pozycje w jednej linii.
        printf("[Napoleon] Stan mapy: ");
        int units_found = 0;

        for (int y = 0; y < MAP_SIZE; y++) {
            pthread_mutex_lock(&map_mutexes[y]);
            for (int x = 0; x < MAP_SIZE; x++) {
                int d_idx = map[y][x];
                if (d_idx != -1) {
                    // Aby odczytać nazwę, musimy na ułamek sekundy zablokować rejestr nazw
                    pthread_mutex_lock(&names_mutex);
                    printf("[%d:%d %s] ", x, y, division_names[d_idx]);
                    pthread_mutex_unlock(&names_mutex);
                    units_found++;
                }
            }
            pthread_mutex_unlock(&map_mutexes[y]);
        }
        
        if (units_found == 0) {
            printf("Brak jednostek na mapie.");
        }
        printf("\n");

        // 3. Wysłanie rozkazu do losowego oddziału sojuszniczego
        pthread_mutex_lock(&names_mutex);
        
        // Zbieramy indeksy wszystkich NASZYCH oddziałów
        int allied_indices[DIVISION_NAMES_SIZE];
        int allied_count = 0;
        
        for (int i = 0; i < division_count; i++) {
            if (division_affiliations[i] == 1) {
                allied_indices[allied_count] = i;
                allied_count++;
            }
        }

        // Jeśli mamy komu wydać rozkaz
        if (allied_count > 0) {
            // Losujemy jeden oddział
            int rand_idx = allied_indices[rand() % allied_count];
            
            // Kopiujemy potrzebne dane lokalnie
            struct sockaddr_in target_addr = division_addrs[rand_idx];
            char target_name[MAX_NAME_LEN];
            strncpy(target_name, division_names[rand_idx], MAX_NAME_LEN);
            
            // ODBLOKOWUJEMY MUTEX PRZED OPERACJĄ SIECIOWĄ (Dobra praktyka)
            pthread_mutex_unlock(&names_mutex);

            // Generujemy nowe współrzędne dla rozkazu
            int new_x = rand() % MAP_SIZE;
            int new_y = rand() % MAP_SIZE;

            char order_msg[MAX_BUFFER];
            int len = snprintf(order_msg, sizeof(order_msg), "%d %d 1 %s\n", new_x, new_y, target_name);

            // Wysyłamy datagram Z POWROTEM do klienta
            sendto(server_sockfd, order_msg, len, 0, (struct sockaddr*)&target_addr, sizeof(target_addr));
            
            printf("[Napoleon] WYSLANO ROZKAZ: Przesun %s na pozycje %d:%d\n", target_name, new_x, new_y);

        } else {
            pthread_mutex_unlock(&names_mutex);
        }
    }
    return NULL;
}

// ==========================================
// KOD ADIUTANTA (Wątek Konsumenta)
// ==========================================
void* adjutant_worker(void* arg) {
    int id = *((int*)arg);
    free(arg);

    while (1) {
        sem_wait(&sem_items);
        
        pthread_mutex_lock(&stack_mutex);
        stack_top--;
        Message msg = message_stack[stack_top];
        pthread_mutex_unlock(&stack_mutex);
        
        sem_post(&sem_spaces);

        int x, y, p;
        char name[MAX_NAME_LEN + 1];
        int parsed_items = sscanf(msg.text, "%d %d %d %128[^\n]", &x, &y, &p, name);

        if (parsed_items != 4 || x < 0 || x >= MAP_SIZE || y < 0 || y >= MAP_SIZE || (p != 0 && p != 1)) {
            continue;
        }

        usleep(10000); // Praca sztabowa

        int div_idx = -1;
        
        // Zapis do rejestru nazw, przynależności oraz ADRESÓW (Etap 4)
        pthread_mutex_lock(&names_mutex);
        for (int i = 0; i < division_count; i++) {
            if (strcmp(division_names[i], name) == 0) {
                div_idx = i;
                break;
            }
        }
        
        if (div_idx == -1 && division_count < DIVISION_NAMES_SIZE) {
            div_idx = division_count;
            strncpy(division_names[div_idx], name, MAX_NAME_LEN);
            division_count++;
        }

        // --- KLUCZOWY ELEMENT ETAPU 4 ---
        if (div_idx != -1) {
            // Niezależnie czy to nowy, czy znany oddział - AKTUALIZUJEMY ADRES NADAWCY
            // Pamiętaj: Jeśli oddział się poruszył z innej stacji nadawczej, zaktualizuje to jego IP/Port
            division_addrs[div_idx] = msg.client_addr;
            division_affiliations[div_idx] = p; // Zapisujemy, czy jest nasz (1), czy wrogi (0)
        }
        pthread_mutex_unlock(&names_mutex);

        if (div_idx == -1) continue; 

        // Aktualizacja mapy
        int old_found = 0;
        for (int row = 0; row < MAP_SIZE; row++) {
            pthread_mutex_lock(&map_mutexes[row]);
            for (int col = 0; col < MAP_SIZE; col++) {
                if (map[row][col] == div_idx) {
                    map[row][col] = -1;
                    old_found = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&map_mutexes[row]);
            if (old_found) break; 
        }

        pthread_mutex_lock(&map_mutexes[y]);
        map[y][x] = div_idx;
        pthread_mutex_unlock(&map_mutexes[y]);
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
    srand(time(NULL)); // Inicjalizacja generatora liczb losowych dla Napoleona

    // Inicjalizacja muteksów mapy i semaforów
    for (int i = 0; i < MAP_SIZE; i++) {
        pthread_mutex_init(&map_mutexes[i], NULL);
        for (int j = 0; j < MAP_SIZE; j++) {
            map[i][j] = -1;
        }
    }
    sem_init(&sem_spaces, 0, STACK_SIZE);
    sem_init(&sem_items, 0, 0);

    // Utworzenie gniazda UDP
    server_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    int opt = 1;
    setsockopt(server_sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        return EXIT_FAILURE;
    }

    // Uruchomienie wątków adiutantów
    pthread_t adjutants[NUM_ADJUTANTS];
    for (int i = 0; i < NUM_ADJUTANTS; i++) {
        int *id = malloc(sizeof(int));
        *id = i + 1;
        pthread_create(&adjutants[i], NULL, adjutant_worker, id);
    }

    // Uruchomienie wątku Napoleona
    pthread_t napoleon_thread;
    pthread_create(&napoleon_thread, NULL, napoleon_worker, NULL);

    printf("\nBitwa trwa! Serwer dziala na porcie %d...\n", port);

    // Główna pętla
    while (1) {
        Message new_msg;
        socklen_t client_len = sizeof(new_msg.client_addr);

        ssize_t n = recvfrom(server_sockfd, new_msg.text, MAX_BUFFER - 1, 0, 
                             (struct sockaddr *)&new_msg.client_addr, &client_len);
                             
        if (n < 0) continue;
        new_msg.text[n] = '\0';

        sem_wait(&sem_spaces);
        pthread_mutex_lock(&stack_mutex);
        message_stack[stack_top++] = new_msg;
        pthread_mutex_unlock(&stack_mutex);
        sem_post(&sem_items);
    }

    return EXIT_SUCCESS;
}