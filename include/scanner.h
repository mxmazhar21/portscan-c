/*
 * Fichier : scanner.h
 * Ce fichier sert à déclarer la partie réseau du scanner : résolution de la
 * cible, test d'un port par connexion TCP non bloquante, récupération des
 * bannières et noms de services.
 */
#ifndef SCANNER_H
#define SCANNER_H

#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>

/* Taille du tampon d'une bannière : 255 caractères + le zéro final. */
#define BANNER_SIZE 256
/* Taille du tampon qui reçoit le nom d'un service ("ssh", "http"...). */
#define SERVICE_SIZE 32
/* Temps d'attente maximum (en ms) d'une bannière après la connexion. */
#define BANNER_WAIT_MS 300
/* Nombre maximum de threads de scan (option -j). */
#define MAX_THREADS 64

/* État d'un port après le test. */
typedef enum {
    PORT_OPEN,     /* la connexion TCP a réussi (SYN-ACK reçu) */
    PORT_CLOSED,   /* la machine a refusé la connexion (RST reçu) */
    PORT_FILTERED  /* pas de réponse dans le délai, ou autre erreur */
} port_state_t;

/* Résultat du test d'un port. */
typedef struct {
    int port;                    /* numéro du port testé */
    port_state_t state;          /* ouvert, fermé ou filtré */
    int scanned;                 /* 1 si le test est allé jusqu'au bout */
    char service[SERVICE_SIZE];  /* nom du service (ports ouverts) */
    char banner[BANNER_SIZE];    /* première ligne envoyée par le service */
} port_result_t;

/* Drapeau levé par le gestionnaire de SIGINT (Ctrl+C) pour arrêter le scan. */
extern volatile sig_atomic_t g_stop_requested;

int resolve_target(const char *host, struct sockaddr_in *address,
                   char *ip_text, size_t ip_text_size);
port_state_t connect_with_timeout(int fd, const struct sockaddr_in *target,
                                  int port, int timeout_ms);
void scan_port(const struct sockaddr_in *target, int timeout_ms,
               port_result_t *result);
port_result_t *create_results(const int *ports, size_t count);
int scan_ports(const struct sockaddr_in *target, port_result_t *results,
               size_t count, int timeout_ms, int thread_count, size_t *tested);
void lookup_service_names(port_result_t *results, size_t count);

#endif
