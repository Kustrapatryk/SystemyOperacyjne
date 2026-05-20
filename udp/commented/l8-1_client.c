#include "l8_common.h" // Dołączenie wspólnego nagłówka z bibliotekami i makrami (np. ERR, TEMP_FAILURE_RETRY)

#define MAXBUF 576 // Definicja maksymalnego rozmiaru przesyłanego datagramu (zgodnie z zadaniem)
volatile sig_atomic_t last_signal = 0; // Zmienna globalna do przechowywania ostatniego sygnału, bezpieczna dla przerwań asynchronicznych

void sigalrm_handler(int sig) { last_signal = sig; } // Procedura obsługi sygnału - zapisuje numer odebranego sygnału (tu: SIGALRM) do zmiennej globalnej

int make_socket() // Funkcja pomocnicza tworząca gniazdo sieciowe
{ // Rozpoczęcie ciała funkcji
    int sock; // Deklaracja zmiennej na deskryptor gniazda
    sock = socket(PF_INET, SOCK_DGRAM, 0); // Utworzenie gniazda IPv4 (PF_INET), datagramowego/bezpołączeniowego (SOCK_DGRAM), czyli UDP
    if (sock < 0) // Sprawdzenie, czy funkcja socket() zwróciła błąd (wartość ujemna)
        ERR("socket"); // Wywołanie makra ERR z l8_common.h, które wypisuje błąd i kończy program
    return sock; // Zwrócenie poprawnego deskryptora gniazda
} // Koniec funkcji

void usage(char *name) { fprintf(stderr, "USAGE: %s domain port file \n", name); } // Funkcja wypisująca prawidłowy sposób użycia programu

void sendAndConfirm(int fd, struct sockaddr_in addr, char *sendbuf, char *recvbuf, ssize_t size) // Funkcja wysyłająca datagram i czekająca na ACK
{ // Rozpoczęcie ciała funkcji
    struct itimerval ts; // Struktura do konfiguracji timera (licznika czasu)
    if (TEMP_FAILURE_RETRY(sendto(fd, sendbuf, size, 0, (struct sockaddr *)&addr, sizeof(addr))) < 0) // Wysłanie danych do serwera (odporne na przerwania dzięki TEMP_FAILURE_RETRY)
        ERR("sendto:"); // Obsługa ewentualnego błędu wysyłania (zabicie programu)
    memset(&ts, 0, sizeof(struct itimerval)); // Wyzerowanie struktury timera, aby uniknąć śmieci w pamięci
    ts.it_value.tv_usec = 500000; // Ustawienie czasu pierwszego wyzwolenia timera na 500 000 mikrosekund (czyli 0,5 sekundy)
    setitimer(ITIMER_REAL, &ts, NULL); // Uruchomienie timera rzeczywistego, który wyśle sygnał SIGALRM po upływie 0,5s
    last_signal = 0; // Wyzerowanie flagi sygnału przed próbą odbioru
    while (recv(fd, recvbuf, size, 0) < 0) // Pętla blokująca, próbująca odebrać potwierdzenie (ACK) od serwera
    { // Rozpoczęcie obsługi błędów odbioru
        if (EINTR != errno) // Jeśli błędem recv NIE było przerwanie przez sygnał (EINTR)
            ERR("recv:"); // Zgłoś krytyczny błąd sieciowy i przerwij program
        if (SIGALRM == last_signal) // Jeśli recv zostało przerwane przez nasz timer (minęło 0,5s)
            break; // Przerwij pętlę i opuść funkcję (pakiet uznany za zgubiony, nastąpi retransmisja w doClient)
    } // Koniec pętli
} // Koniec funkcji

void doClient(int fd, struct sockaddr_in addr, int file) // Główna logika klienta przesyłającego plik
{ // Rozpoczęcie ciała funkcji
    char sendbuf[MAXBUF]; // Bufor na dane do wysłania (576 bajtów)
    char recvbuf[MAXBUF]; // Bufor na dane odebrane - potwierdzenia (576 bajtów)
    int offset = 2 * sizeof(int32_t); // Obliczenie przesunięcia dla tekstu (8 bajtów), robimy miejsce na dwie liczby int32_t (nr paczki i flaga końca)
    int32_t chunkNo = 0; // Licznik wysłanych fragmentów (paczek), inicjowany na 0
    ssize_t size; // Zmienna przechowująca ilość faktycznie wczytanych bajtów z pliku
    int counter; // Licznik ponownych prób wysłania tego samego fragmentu (retransmisji)
    do // Rozpoczęcie pętli wysyłającej cały plik, fragment po fragmencie
    { // Ciało pętli głównej
        memset(sendbuf, 0, MAXBUF); // Czyszczenie bufora nadawczego (ważne, bo dzięki temu flaga "czy to koniec" jest domyślnie 0)
        memset(recvbuf, 0, MAXBUF); // Czyszczenie bufora odbiorczego przed nowym odczytem

        if ((size = bulk_read(file, sendbuf + offset, MAXBUF - offset)) < 0) // Wczytywanie z pliku bezpośrednio do bufora (za 8 bajtem metadanych). bulk_read dba o doczytanie pełnego bloku.
            ERR("read from file:"); // Zakończenie programu w wypadku błędu odczytu z dysku
        *((int32_t *)sendbuf) = htonl(++chunkNo); // Rzutowanie początku bufora na wskaźnik int32_t i zapisanie tam numeru paczki (skonwertowanego na sieciowy porządek bajtów)
        if (size < MAXBUF - offset) // Sprawdzenie, czy z pliku wczytano mniej znaków niż wynosi maksymalna pojemność (oznacza to, że dotarliśmy do końca pliku)
        { // Rozpoczęcie obsługi ostatniej paczki
            memset(sendbuf + offset + size, 0, MAXBUF - offset - size); // Wyzerowanie reszty bufora, na wypadek gdyby zostały tam śmieci
            *(((int32_t *)sendbuf) + 1) = htonl(1); // Ustawienie flagi "ostatnia paczka" na 1 (znajduje się ona 4 bajty po numerze paczki)
        } // Koniec bloku obsługi ostatniej paczki
        counter = 0; // Wyzerowanie licznika prób dla obecnego datagramu

        do // Wewnętrzna pętla odpowiedzialna za wysyłanie jednej paczki i ponawianie w razie braku ACK
        { // Ciało pętli retransmisji
            counter++; // Zwiększenie licznika prób
            sendAndConfirm(fd, addr, sendbuf, recvbuf, MAXBUF); // Próba wysłania paczki i oczekiwanie 0,5s na odpowiedź
        } while (*((int32_t *)recvbuf) != (int32_t)htonl(chunkNo) && counter <= 5); // Powtarzaj dopóki odebrany numer paczki w ACK nie zgadza się z wysłanym ORAZ nie przekroczono 5 prób

        if (*((int32_t *)recvbuf) != (int32_t)htonl(chunkNo) && counter > 5) // Jeśli mimo 5 retransmisji nie otrzymaliśmy poprawnego potwierdzenia
            break; // Przerwij główną pętlę i zakończ transfer (wymóg zadania: po 5 niepowodzeniach zakończ)

    } while (size == MAXBUF - offset); // Kontynuuj czytanie i wysyłanie kolejnych fragmentów, dopóki wczytujemy pełne pakiety z pliku
} // Koniec funkcji doClient

int main(int argc, char **argv) // Punkt wejścia programu
{ // Rozpoczęcie funkcji main
    int fd, file; // Deklaracja zmiennych na deskryptor gniazda i deskryptor otwieranego pliku tekstowego
    struct sockaddr_in addr; // Deklaracja struktury przechowującej adres serwera
    if (argc != 4) // Sprawdzenie, czy podano dokładnie 3 argumenty (nazwa programu, IP, port, plik)
    { // Obsługa złej liczby argumentów
        usage(argv[0]); // Wypisanie podpowiedzi dla użytkownika
        return EXIT_FAILURE; // Zakończenie programu z kodem błędu
    } // Koniec bloku sprawdzającego argumenty
    if (sethandler(SIG_IGN, SIGPIPE)) // Ignorowanie sygnału SIGPIPE (zapobiega ubiciu programu przy zerwanym połączeniu zapisu)
        ERR("Seting SIGPIPE:"); // Zgłoszenie błędu jeśli nie udało się ustawić obsługi sygnału
    if (sethandler(sigalrm_handler, SIGALRM)) // Podpięcie naszej funkcji sigalrm_handler do sygnału SIGALRM (obsługa timeoutu)
        ERR("Seting SIGALRM:"); // Zgłoszenie błędu konfiguracji sygnału
    if ((file = TEMP_FAILURE_RETRY(open(argv[3], O_RDONLY))) < 0) // Otwarcie pliku podanego jako argument w trybie tylko do odczytu
        ERR("open:"); // Przerwanie jeśli pliku nie ma lub brak uprawnień
    fd = make_socket(); // Utworzenie lokalnego gniazda UDP
    addr = make_address(argv[1], argv[2]); // Zbudowanie struktury z adresem IP (argv[1]) i portem (argv[2]) serwera przy pomocy getaddrinfo
    doClient(fd, addr, file); // Wywołanie głównej funkcji komunikacyjnej klienta
    if (TEMP_FAILURE_RETRY(close(fd)) < 0) // Poprawne zamknięcie gniazda sieciowego po zakończeniu komunikacji
        ERR("close"); // Obsługa ewentualnego błędu przy zamykaniu
    if (TEMP_FAILURE_RETRY(close(file)) < 0) // Zamknięcie deskryptora pliku tekstowego
        ERR("close"); // Obsługa ewentualnego błędu zamykania pliku
    return EXIT_SUCCESS; // Zakończenie programu z kodem sukcesu (0)
} // Koniec funkcji main