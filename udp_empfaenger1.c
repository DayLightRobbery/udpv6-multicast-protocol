#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h>

#define DEFAULT_MULTICAST_ADDR "ff02::1" // Standard-Multicast-Adresse
#define PORT 12345                      // Standard-Port
#define MAX_SEQ_NUM 100                 // Maximale Sequenznummer
#define DEFAULT_WINDOW_SIZE 5           // Standard-Fenstergröße
#define TIMEOUT_INT 300                 // Timeout in Millisekunden
#define MAX_RETRIES 3                   // Maximale Wiederholungen

// ACK für ein Paket senden
void sende_ack(int sock, struct sockaddr_in6 *adresse, int seq_num) {
    char ack[2048];
    sprintf(ack, "ACK %d", seq_num);
    sendto(sock, ack, strlen(ack), 0, (struct sockaddr *)adresse, sizeof(*adresse));
    printf("[EMPFÄNGER] ACK gesendet für Paket %d\n", seq_num);
}

// ACK_CLOSE senden
void sende_close_ack(int sock, struct sockaddr_in6 *adresse) {
    const char *close_ack = "ACK_CLOSE";
    sendto(sock, close_ack, strlen(close_ack), 0, (struct sockaddr *)adresse, sizeof(*adresse));
    printf("[EMPFÄNGER] ACK_CLOSE gesendet. Übertragung abgeschlossen.\n");
}

int main(int argc, char *argv[]) {
    char multicast_addr[50] = DEFAULT_MULTICAST_ADDR; // Standard-Multicast-Adresse
    char dateiname[50] = "empfangen.txt";             // Standard-Dateiname
    int fenster_size = DEFAULT_WINDOW_SIZE;           // Standard-Fenstergröße

    // Argumente verarbeiten
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            strncpy(multicast_addr, argv[i + 1], sizeof(multicast_addr) - 1);
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            fenster_size = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            strncpy(dateiname, argv[i + 1], sizeof(dateiname) - 1);
        }
    }

    printf("[EMPFÄNGER] Multicast-Adresse: %s, Fenstergröße: %d, Dateiname: %s\n", multicast_addr, fenster_size, dateiname);

    int sock;
    struct sockaddr_in6 adresse, sender_adresse;
    socklen_t sender_adresse_len = sizeof(sender_adresse);
    char paket[1024];
    int fenster_start = 0, fenster_end = fenster_size - 1;
    int empfangen[MAX_SEQ_NUM] = {0}; // Status der empfangenen Pakete
    char daten[MAX_SEQ_NUM][1024] = {{0}};
    FILE *datei;

    // Socket erstellen
    sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Socket konnte nicht erstellt werden");
        return 1;
    }

    // Lokale Adresse binden
    memset(&adresse, 0, sizeof(adresse));
    adresse.sin6_family = AF_INET6;
    adresse.sin6_addr = in6addr_any;
    adresse.sin6_port = htons(PORT);

    if (bind(sock, (struct sockaddr *)&adresse, sizeof(adresse)) < 0) {
        perror("Bind fehlgeschlagen");
        close(sock);
        return 1;
    }

    // Multicast-Gruppe beitreten
    struct ipv6_mreq mreq;
    inet_pton(AF_INET6, multicast_addr, &mreq.ipv6mr_multiaddr);
    mreq.ipv6mr_interface = 0;
    setsockopt(sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    // Datei öffnen
    datei = fopen(dateiname, "w");
    if (!datei) {
        perror("Datei konnte nicht geöffnet werden");
        close(sock);
        return 1;
    }

    printf("[EMPFÄNGER] Warte auf Pakete...\n");

    struct timeval timeout;
    fd_set readfds;
    int retries = 0;

    // Hauptschleife
    while (1) {
        timeout.tv_sec = 0;
        timeout.tv_usec = TIMEOUT_INT * 1000;

        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        int sel_res = select(sock + 1, &readfds, NULL, NULL, &timeout);

        if (sel_res > 0) { // Paket empfangen
            memset(paket, 0, sizeof(paket));
            int anzahl = recvfrom(sock, paket, sizeof(paket) - 1, 0, (struct sockaddr *)&sender_adresse, &sender_adresse_len);
            if (anzahl < 0) {
                perror("Fehler beim Empfang");
                break;
            }

            paket[anzahl] = '\0';
            int seq_num;
            char daten_paket[1024];

            // CLOSE-Nachricht empfangen
            if (strcmp(paket, "CLOSE") == 0) {
                printf("[EMPFÄNGER] CLOSE-Nachricht empfangen. Sende ACK_CLOSE...\n");
                sende_close_ack(sock, &sender_adresse);
                break;
            }

            // Paket mit Sequenznummer empfangen
            if (sscanf(paket, "%d:%[^\n]", &seq_num, daten_paket) != 2) continue;

            printf("[EMPFÄNGER] Paket %d empfangen: %s\n", seq_num, daten_paket);

            // Paket in Empfangspuffer speichern
            if (!empfangen[seq_num]) {
                strcpy(daten[seq_num], daten_paket);
                empfangen[seq_num] = 1;
                sende_ack(sock, &sender_adresse, seq_num);

                // Pakete in Datei schreiben
                while (fenster_start < MAX_SEQ_NUM && empfangen[fenster_start]) {
                    fprintf(datei, "%s\n", daten[fenster_start]);
                    fflush(datei);
                    printf("[EMPFÄNGER] Paket %d in Datei geschrieben\n", fenster_start);
                    fenster_start++;
                    fenster_end = fenster_start + fenster_size - 1;
                }
            }
        } else if (sel_res == 0) { // Timeout
            retries++;
            if (retries >= MAX_RETRIES) {
                printf("[EMPFÄNGER] Keine neuen Pakete. Maximale Wiederholungen erreicht. Beende.\n");
                break;
            }
        } else {
            perror("Fehler bei select()");
            break;
        }
    }

    fclose(datei);
    close(sock);
    printf("[EMPFÄNGER] Programm beendet.\n");
    return 0;
}
