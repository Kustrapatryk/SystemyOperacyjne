#include "l8_common.h" // Dołączenie wspólnych narzędzi, nagłówków i obsługi błędów

#define BACKLOG 3 // Domyślna wielkość kolejki dla listen() - w tym zadaniu nieużywane, bo używamy UDP (które nie ma listen/accept)
#define MAXBUF 576 // Maksymalny rozmiar datagramu do odebrania
#define MAXADDR 5 // Maksymalna ilość równoległych klientów obsługiwanych przez serwer (wymóg z zadania)

struct connections // Struktura przechowująca stan pojedynczej "sesji" (ponieważ UDP jest bezstanowe, serwer musi sam śledzić stan)
{ // Rozpoczęcie struktury
    int free; // Flaga określająca, czy to miejsce w tablicy jest wolne (1) czy zajęte (0)
    int32_t chunkNo; // Numer ostatnio pomyślnie odebranego fragmentu dla danego klienta
    struct sockaddr_in addr; // Adres IP oraz port klienta z którym aktualnie rozmawiamy
}; // Koniec struktury

int make_socket(int domain, int type) // Funkcja pomocnicza tworząca gniazdo na podanych parametrach
{ // Rozpoczęcie ciała funkcji
    int sock; // Deklaracja zmiennej trzymającej deskryptor
    sock = socket(domain, type, 0); // Odpytanie systemu o nowe gniazdo
    if (sock < 0) // Jeśli system odmówi (zwróci -1)
        ERR("socket"); // Przerwij działanie programu serwera
    return sock; // Zwróć utworzony deskryptor
} // Koniec funkcji

int bind_inet_socket(uint16_t port, int type) // Funkcja przypisująca numer portu do gniazda serwera
{ // Rozpoczęcie ciała funkcji
    struct sockaddr_in addr; // Lokalna zmienna do przechowania konfiguracji adresu serwera
    int socketfd, t = 1; // socketfd do trzymania deskryptora, t używane jako prawda (1) w opcjach socketu
    socketfd = make_socket(PF_INET, type); // Stworzenie gniazda IPv4 o zadanym typie (w naszym przypadku DGRAM)
    memset(&addr, 0, sizeof(struct sockaddr_in)); // Wyzerowanie struktury adresu
    addr.sin_family = AF_INET; // Ustalenie rodziny adresów na IPv4
    addr.sin_port = htons(port); // Przypisanie portu, po uprzednim zamienieniu bajtów na standard sieciowy
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // Nasłuchiwanie na wszystkich interfejsach sieciowych serwera (0.0.0.0)
    if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &t, sizeof(t))) // Zabezpieczenie przed błędem "Address already in use" przy szybkim restarcie serwera
        ERR("setsockopt"); // Reagowanie na błąd przypisania opcji
    if (bind(socketfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) // Rzeczywiste "zbindowanie", czyli przypięcie gniazda do portu w OS
        ERR("bind"); // Przerwanie jeśli port jest zajęty lub brak uprawnień
    if (SOCK_STREAM == type) // Sprawdzenie, czy gniazdo było strumieniowe (TCP)
        if (listen(socketfd, BACKLOG) < 0) // Jeśli tak, to włącza nasłuchiwanie (nie dotyczy naszego wywołania UDP z maina)
            ERR("listen"); // Przerwanie w wypadku błędu listen
    return socketfd; // Zwrócenie w pełni skonfigurowanego i zbindowanego gniazda
} // Koniec funkcji

int findIndex(struct sockaddr_in addr, struct connections con[MAXADDR]) // Funkcja szukająca, z którym klientem właśnie rozmawiamy, lub przydzielająca nowego
{ // Rozpoczęcie ciała funkcji
    int i, empty = -1, pos = -1; // i - iterator, empty - indeks pierwszego wolnego slotu, pos - indeks znalezionego znanego klienta
    for (i = 0; i < MAXADDR; i++) // Pętla przeszukująca maksymalnie 5 dozwolonych sesji
    { // Rozpoczęcie iteracji
        if (con[i].free) // Sprawdza, czy slot na indeksie 'i' jest niezajęty
            empty = i; // Zapamiętanie ostatniego napotkanego wolnego miejsca
        else if (0 == memcmp(&addr, &(con[i].addr), sizeof(struct sockaddr_in))) // Jeśli slot jest zajęty, porównuje binarnie adres klienta z przychodzącym pakietem
        { // Jeśli adres IP i port klienta są w tablicy
            pos = i; // Znaleźliśmy aktywną sesję klienta, zapisujemy pozycję
            break; // Przerywamy szukanie, nie ma sensu dalej przeglądać tablicy
        } // Koniec warunku znalezienia
    } // Koniec pętli przeszukującej tablicę
    if (-1 == pos && empty != -1) // Jeśli nie znaleziono dopasowania (pos to nadal -1), ale znalazło się wolne miejsce (empty != -1)
    { // Inicjalizacja nowej sesji
        con[empty].free = 0; // Oznaczenie slota jako zajęty
        con[empty].chunkNo = 0; // Wyzerowanie licznika odebranych pakietów dla nowego klienta
        con[empty].addr = addr; // Przypisanie adresu nowego klienta do tablicy sesji
        pos = empty; // Ustawienie indeksu na ten nowo utworzony
    } // Koniec bloku tworzenia nowej sesji
    return pos; // Zwraca pozycję w tablicy. Jeśli nie było klienta i nie było miejsca (ponad 5 połączeń), zwróci -1
} // Koniec funkcji

void doServer(int fd) // Główna pętla serwera obsługująca ruch
{ // Rozpoczęcie ciała funkcji
    struct sockaddr_in addr; // Struktura, do której kernel wpisze adres klienta przysyłającego datagram
    struct connections con[MAXADDR]; // Utworzenie tablicy na 5 niezależnych sesji klienckich
    char buf[MAXBUF + 1]; // Bufor na przychodzące dane + 1 bajt na terminujący znak \0 potrzebny np. do printf
    for (int i = 0; i < MAXADDR; i++) // Pętla początkowa konfigurująca tablicę
        con[i].free = 1; // Oznaczenie wszystkich sesji jako początkowo całkowicie puste (free = 1)

    while (1) // Nieskończona pętla serwera - serwer wisi i nasłuchuje przez cały czas działania
    { // Główna logika odbioru
        socklen_t size = sizeof(addr); // Inicjalizacja parametru length potrzebnego do value-result w recvfrom
        int receivedBytes; // Zmienna na ilość faktycznie odebranych bajtów z sieci
        if ((receivedBytes = TEMP_FAILURE_RETRY(recvfrom(fd, buf, MAXBUF, 0, (struct sockaddr *)&addr, &size))) < 0) // Oczekiwanie na jakikolwiek pakiet, zapisuje kto wysłał do zmiennej addr
            ERR("read:"); // Reaguje zabiciem procesu, jeśli system zgłosi krytyczny błąd gniazda
        buf[receivedBytes] = 0; // Doklejenie sztucznego zera na końcu odebranych bajtów, zabezpieczając użycie printf() na danych jako c-stringach
        int index = -1; // Domyślny indeks sesji przed przeszukaniem

        if ((index = findIndex(addr, con)) >= 0) // Jeśli w tablicy jest miejsce na nowe połączenie lub klient już w niej widnieje (odrzuci jeśli > 5 równoległych)
        { // Analiza zweryfikowanego i dopuszczonego pakietu
            int32_t chunkNo = ntohl(*((int32_t *)buf)); // Odczytanie 4 pierwszych bajtów z bufora, konwersja do porządku hosta i wyciągnięcie numeru paczki
            if (chunkNo > con[index].chunkNo + 1) // Jeśli pakiet ubiegł poprzednie pakiety (teoretycznie przy retransmisjach się to w tym kodzie nie stanie, ale to pętla ochronna)
            { // Jeśli brakuje nam jakiegoś wcześniejszego numeru
                continue; // Zignoruj ten pakiet całkowicie i przejdź do nasłuchiwania kolejnych datagramów z sieci (niczego nie odsyłaj!)
            } // Koniec zabezpieczenia przed złą kolejnością
            else if (chunkNo == con[index].chunkNo + 1) // Jeśli to dokładnie ta paczka, na którą czekał ten konkretny klient (kolejność się zgadza)
            { // Przetwarzanie nowej, spodziewanej paczki
                if (ntohl(*(((int32_t *)buf) + 1))) // Czytamy 4 bajty po numerze paczki (przesuwamy wskaźnik int32_t o 1) - sprawdzamy flagę "ostatniej wiadomości"
                { // Jeśli flaga ma wartość 1 (ostatni datagram pliku)
                    printf("Last Part %d\n%s\n", chunkNo, buf + 2 * sizeof(int32_t)); // Wypisanie informacji i zawartości tekstu z pliku (omijając pierwsze 8 bajtów sterujących)
                    con[index].free = 1; // Zwalniamy slot po tym kliencie, zamykając sesję, aby udostępnić miejsce dla innych nowo podłączonych
                } // Koniec bloku ostatniego datagramu
                else // W przeciwnym razie, flaga była 0 (zwykły fragment pliku)
                { // Jeśli to zwykły datagram
                    printf("Part %d\n%s\n", chunkNo, buf + 2 * sizeof(int32_t)); // Wypisz na serwerze jego tekstową zawartość pomijając 8 bajtów metadanych
                } // Koniec wypisywania zwykłego
                con[index].chunkNo++; // Serwer zanotował przyjęcie paczki - zwiększa swój licznik dla danego klienta
            } // Zauważ: pominięto warunek "chunkNo <= con[index].chunkNo" - takie pakiety to zduplikowane retransmisje. Program omija ich przetwarzanie (NIE WYPISUJE ich drugi raz na ekran)...

            if (TEMP_FAILURE_RETRY(sendto(fd, buf, MAXBUF, 0, (struct sockaddr *)&addr, size)) < 0) // ... ale niezależnie od tego czy to paczka nowa, czy spóźniony duplikat, ODSYŁA ten sam bufor klientowi jako ACK!
            { // Jeśli kernel zgłosi błąd przy próbie wypchnięcia paczki (ACK)
                if (EPIPE == errno) // Jeżeli rura jest przerwana (dla UDP rzadkie, ale poprawne systemowo zabezpieczenie)
                    con[index].free = 1; // Uznaje, że klient zniknął na dobre, i zwalnia jego slot
                else // Każdy inny błąd (np. brak pamięci jądra)
                    ERR("send:"); // Przerywa działanie całego serwera
            } // Koniec bloku obsługi błędów odsyłania ACK
        } // Koniec bloku operacji na aktywnym połączeniu (pakiety od 6. i dalszych klientów są całkowicie połykane i ignorowane bez ACK)
    } // Koniec nieskończonej pętli nasłuchiwania
} // Koniec głównej funkcji serwera

void usage(char *name) { fprintf(stderr, "USAGE: %s port\n", name); } // Drukuje pomoc, jeśli argumenty będą się nie zgadzać

int main(int argc, char **argv) // Główny punkt programu serwera
{ // Rozpoczęcie maina
    int fd; // Deskryptor, który przechowa gniazdo serwera
    if (argc != 2) // Oczekuje dokładnie 2 elementów startowych: [nazwa_programu] [port]
    { // Jeśli użytkownik popełnił błąd
        usage(argv[0]); // Zostanie upomniany
        return EXIT_FAILURE; // I program od razu się zamknie z kodem błędu
    } // Koniec weryfikacji startowej
    if (sethandler(SIG_IGN, SIGPIPE)) // Neutralizacja sygnału zrywanego potoku (SIGPIPE) - dla świętego spokoju, by OS nie ubił procesu niespodziewanie
        ERR("Seting SIGPIPE:"); // Błąd w ustawieniu pułapki na sygnał
    fd = bind_inet_socket(atoi(argv[1]), SOCK_DGRAM); // Odczytanie portu z wejścia (atoi), stworzenie gniazda UDP (SOCK_DGRAM) i podpięcie go do systemu operacyjnego
    doServer(fd); // Odpalenie ciągłej, nieskończonej pętli roboczej na tym gnieździe (blokuje działanie)
    if (TEMP_FAILURE_RETRY(close(fd)) < 0) // Linia nigdy nie zostanie osiągnięta naturalnie, ale gdyby wyrwano serwer z while(1), zamyka kulturalnie gniazdo
        ERR("close"); // Jeśli OS rzuci błędem zamykania, loguje go i rzuca krytyk
    fprintf(stderr, "Server has terminated.\n"); // Informacyjnie potwierdza zakończenie pracy
    return EXIT_SUCCESS; // Oddaje systemowi operacyjnemu kod 0 jako powiadomienie o sukcesie
} // Koniec funkcji main