#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h>

#define DEFAULT_WINDOW_SIZE 5    // Standard-Fenstergröße
#define TIMEOUT_MS 300           // Timeout in Millisekunden
#define MAX_RETRIES 5            // Maximale Anzahl von Wiederholungen
#define MAX_SEQ_NUM 100          // Maximale Sequenznummer

typedef enum {IDLE, CONNECTION_PREPARE, CONNECTION_ESTABLISHED, CLOSE_PREPARE} State;

// Zustände ausgeben
void print_state(State state) {
    const char *state_names[] = {"IDLE", "CONNECTION_PREPARE", "CONNECTION_ESTABLISHED", "CLOSE_PREPARE"};
    printf("\n--- Aktueller Zustand: %s ---\n", state_names[state]);
}

int main(int argc, char *argv[]) {
    char multicast_addr[50] = "FF02::1";  // Standard-Multicast-Adresse
    int window_size = DEFAULT_WINDOW_SIZE;
    char filename[100] = "daten.txt";

    // Argumente verarbeiten
    if (argc < 7) {
        fprintf(stderr, "Nutzung: %s -a <Multicast-Adresse> -w <Fenstergroesse> -f <Dateiname>\n", argv[0]);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            strncpy(multicast_addr, argv[i + 1], sizeof(multicast_addr) - 1);
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            window_size = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            strncpy(filename, argv[i + 1], sizeof(filename) - 1);
        }
    }

    printf("[SENDER] Multicast-Adresse: %s, Fenstergröße: %d, Datei: %s\n", multicast_addr, window_size, filename);

    int sock;
    struct sockaddr_in6 adresse;
    socklen_t adresse_len = sizeof(adresse);
    FILE *datei;
    char daten[1024];
    int seq_num = 0; // Sequenznummer
    State state = IDLE;

    // UDP-Socket erstellen
    sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Socket konnte nicht erstellt werden");
        return 1;
    }

    memset(&adresse, 0, sizeof(adresse));
    adresse.sin6_family = AF_INET6;
    adresse.sin6_port = htons(12345);
    if (inet_pton(AF_INET6, multicast_addr, &adresse.sin6_addr) <= 0) {
        perror("Ungültige Multicast-Adresse");
        close(sock);
        return 1;
    }

    // HELLO-Phase
    state = CONNECTION_PREPARE;
    print_state(state);
    sendto(sock, "HELLO", 5, 0, (struct sockaddr *)&adresse, adresse_len);
    printf("[SENDER] HELLO-Nachricht gesendet.\n");

    fd_set readfds;
    struct timeval timeout;
    char empfang[1024];

    timeout.tv_sec = 0;
    timeout.tv_usec = TIMEOUT_MS * 1000 * 3;

    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    int res = select(sock + 1, &readfds, NULL, NULL, &timeout);

    if (res > 0) {
        recvfrom(sock, empfang, sizeof(empfang) - 1, 0, (struct sockaddr *)&adresse, &adresse_len);
        if (strcmp(empfang, "ACK_HELLO") == 0) {
            printf("[SENDER] ACK_HELLO empfangen.\n");
        }
    } else {
        printf("[SENDER] Keine Antwort auf HELLO. Abbruch.\n");
        close(sock);
        return 1;
    }

    state = CONNECTION_ESTABLISHED;
    print_state(state);

    // Datei öffnen
    datei = fopen(filename, "r");
    if (!datei) {
        perror("Datei konnte nicht geöffnet werden");
        close(sock);
        return 1;
    }

    int acked[MAX_SEQ_NUM] = {0}, retry_count[MAX_SEQ_NUM] = {0};
    int fenster_start = 0, fenster_end = window_size - 1;
    int alles_bestätigt = 0;

    while (!alles_bestätigt) {
        for (int i = fenster_start; i <= fenster_end && !feof(datei); i++) {
            if (!acked[i] && retry_count[i] < MAX_RETRIES) {
                if (fgets(daten, sizeof(daten), datei)) {
                    char paket[2048];
                    snprintf(paket, sizeof(paket), "%d:%s", i, daten);
                    sendto(sock, paket, strlen(paket), 0, (struct sockaddr *)&adresse, adresse_len);
                    printf("[SENDER] Paket %d gesendet: %s", i, daten);
                    retry_count[i]++;
                }
            }
        }

        timeout.tv_usec = TIMEOUT_MS * 1000;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        res = select(sock + 1, &readfds, NULL, NULL, &timeout);
        if (res > 0) {
            int len = recvfrom(sock, empfang, sizeof(empfang) - 1, 0, (struct sockaddr *)&adresse, &adresse_len);
            empfang[len] = '\0';
            int ack_num;
            if (sscanf(empfang, "ACK %d", &ack_num) == 1) {
                acked[ack_num] = 1;
                while (fenster_start <= fenster_end && acked[fenster_start]) {
                    fenster_start++;
                    fenster_end = fenster_start + window_size - 1;
                }
            }
        }
    }

    // CLOSE-Phase
    state = CLOSE_PREPARE;
    print_state(state);
    sendto(sock, "CLOSE", 5, 0, (struct sockaddr *)&adresse, adresse_len);
    printf("[SENDER] CLOSE-Nachricht gesendet. Warte auf ACK_CLOSE...\n");

    for (int i = 0; i < MAX_RETRIES; i++) {
        res = select(sock + 1, &readfds, NULL, NULL, &timeout);
        if (res > 0) {
            recvfrom(sock, empfang, sizeof(empfang) - 1, 0, (struct sockaddr *)&adresse, &adresse_len);
            empfang[res] = '\0';
            if (strcmp(empfang, "ACK_CLOSE") == 0) {
                printf("[SENDER] ACK_CLOSE empfangen. Programm beendet.\n");
                break;
            }
        } else {
            sendto(sock, "CLOSE", 5, 0, (struct sockaddr *)&adresse, adresse_len);
            printf("[SENDER] CLOSE erneut gesendet.\n");
        }
    }

    fclose(datei);
    close(sock);
    state = IDLE;
    print_state(state);
    return 0;
}
