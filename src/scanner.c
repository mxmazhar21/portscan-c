/*
 * Fichier : scanner.c
 * Ce fichier sert à tester les ports : pour chaque port on crée un socket
 * non bloquant, on lance connect, on attend la réponse avec select, puis on
 * classe le port (ouvert, fermé, filtré). Pour un port ouvert, on essaie de
 * lire la bannière du service avant de fermer le socket. Avec l'option -j,
 * plusieurs threads se partagent la liste des ports.
 */
#include "scanner.h"
#include "utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Drapeau d'arrêt : mis à 1 par le gestionnaire de SIGINT (voir main.c). */
volatile sig_atomic_t g_stop_requested = 0;

/* Ports web : si le service ne parle pas en premier, on lui envoie une
 * requête HTTP pour obtenir une réponse. */
static const int HTTP_PORTS[] = { 80, 8000, 8080, 8888 };

/* Nombre de cases du tableau HTTP_PORTS. */
static const size_t HTTP_PORT_COUNT = sizeof(HTTP_PORTS) / sizeof(HTTP_PORTS[0]);

/* Requête HTTP minimale : HEAD demande seulement les en-têtes. */
static const char HTTP_REQUEST[] = "HEAD / HTTP/1.0\r\n\r\n";

/* Travail partagé entre les threads de scan. */
typedef struct {
    const struct sockaddr_in *target; /* adresse de la cible */
    port_result_t *results;           /* une case par port, préallouée */
    size_t count;                     /* nombre de ports à tester */
    int timeout_ms;                   /* délai maximum par port */
    size_t next_index;                /* prochain port à distribuer */
    pthread_mutex_t lock;             /* verrou qui protège next_index */
} scan_job_t;

/*
 * Traduit le nom de la cible (ou une IP écrite en texte) en adresse IPv4.
 * Paramètres : host = nom ou IP, address = reçoit l'adresse,
 *              ip_text/ip_text_size = reçoit l'IP en texte "a.b.c.d".
 * Retour : 0 si succès, sinon le code d'erreur de getaddrinfo (à afficher
 *          avec gai_strerror).
 */
int resolve_target(const char *host, struct sockaddr_in *address,
                   char *ip_text, size_t ip_text_size)
{
    struct addrinfo hints;
    struct addrinfo *found = NULL;
    int status;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       /* IPv4 uniquement */
    hints.ai_socktype = SOCK_STREAM; /* TCP */
    /* getaddrinfo résout le nom (DNS, /etc/hosts...) ; renvoie 0 si succès,
     * sinon un code EAI_xxx (ex. EAI_NONAME : nom inconnu). */
    status = getaddrinfo(host, NULL, &hints, &found);
    if (status != 0) {
        return status;
    }
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_addr = ((const struct sockaddr_in *)found->ai_addr)->sin_addr;
    /* freeaddrinfo libère la liste chaînée allouée par getaddrinfo. */
    freeaddrinfo(found);
    /* inet_ntop écrit l'adresse en texte ; NULL si le tampon est trop petit. */
    if (inet_ntop(AF_INET, &address->sin_addr, ip_text,
                  (socklen_t)ip_text_size) == NULL) {
        snprintf(ip_text, ip_text_size, "?");
    }
    return 0;
}

/*
 * Crée un socket TCP IPv4 en mode non bloquant.
 * Retour : le descripteur du socket, ou -1 en cas d'erreur.
 */
static int create_nonblocking_socket(void)
{
    int fd;
    int flags;

    /* socket crée un point de communication TCP ; renvoie un descripteur
     * >= 0, ou -1 (ex. EMFILE : trop de fichiers ouverts). */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    /* fcntl(F_GETFL) lit les options actuelles du descripteur ; -1 si
     * erreur. */
    flags = fcntl(fd, F_GETFL, 0);
    /* fcntl(F_SETFL) ajoute O_NONBLOCK : connect et recv ne bloqueront
     * plus le programme ; -1 si erreur. */
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * Calcule l'instant limite "maintenant + timeout_ms".
 * Paramètres : timeout_ms = délai en millisecondes, deadline = résultat.
 */
static void compute_deadline(int timeout_ms, struct timespec *deadline)
{
    get_monotonic_time(deadline);
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec += 1;
        deadline->tv_nsec -= 1000000000L;
    }
}

/*
 * Calcule le temps restant avant l'instant limite.
 * Paramètre : deadline = instant limite.
 * Retour : nombre de millisecondes restantes (0 si l'instant est passé).
 */
static long remaining_ms(const struct timespec *deadline)
{
    struct timespec now;
    long left;

    get_monotonic_time(&now);
    left = (long)(deadline->tv_sec - now.tv_sec) * 1000L
           + (deadline->tv_nsec - now.tv_nsec) / 1000000L;
    return (left > 0) ? left : 0;
}

/*
 * Attend qu'un socket soit prêt, au plus timeout_ms millisecondes.
 * Paramètres : fd = socket, want_write = 1 pour attendre la fin de connect
 *              (écriture possible), 0 pour attendre des données (lecture),
 *              timeout_ms = délai maximum.
 * Retour : 1 si le socket est prêt, 0 si le délai est dépassé, -1 en cas
 *          d'erreur ou si l'utilisateur a demandé l'arrêt (Ctrl+C).
 */
static int wait_for_fd(int fd, int want_write, int timeout_ms)
{
    struct timespec deadline;

    compute_deadline(timeout_ms, &deadline);
    for (;;) {
        fd_set set;
        struct timeval delay;
        long left = remaining_ms(&deadline);
        int ready;

        FD_ZERO(&set);
        FD_SET(fd, &set);
        delay.tv_sec = left / 1000;
        delay.tv_usec = (left % 1000) * 1000;
        /* select attend que le socket soit prêt (ou la fin du délai) ;
         * renvoie > 0 si prêt, 0 si délai dépassé, -1 en cas d'erreur
         * (errno = EINTR si un signal l'a interrompu). */
        ready = select(fd + 1, want_write ? NULL : &set,
                       want_write ? &set : NULL, NULL, &delay);
        if (ready >= 0) {
            return (ready > 0) ? 1 : 0;
        }
        if (errno != EINTR || g_stop_requested) {
            return -1;
        }
    }
}

/*
 * Classe un port à partir du code d'erreur de la connexion.
 * Paramètre : error = 0 si la connexion a réussi, sinon un code errno.
 * Retour : PORT_OPEN, PORT_CLOSED ou PORT_FILTERED.
 */
static port_state_t state_from_error(int error)
{
    if (error == 0) {
        return PORT_OPEN;
    }
    if (error == ECONNREFUSED) {
        return PORT_CLOSED;
    }
    return PORT_FILTERED;
}

/*
 * Attend la fin d'une connexion en cours puis lit son résultat réel.
 * Paramètres : fd = socket en cours de connexion, timeout_ms = délai.
 * Retour : état du port.
 */
static port_state_t finish_pending_connection(int fd, int timeout_ms)
{
    int socket_error = 0;
    socklen_t length = sizeof(socket_error);

    if (wait_for_fd(fd, 1, timeout_ms) != 1) {
        return PORT_FILTERED;
    }
    /* getsockopt(SO_ERROR) donne le résultat de la connexion : 0 si elle a
     * réussi, sinon le code d'erreur (ECONNREFUSED, ETIMEDOUT...) ; l'appel
     * lui-même renvoie -1 s'il échoue. */
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) < 0) {
        return PORT_FILTERED;
    }
    return state_from_error(socket_error);
}

/*
 * Tente une connexion TCP sur un port avec un délai maximum.
 * Paramètres : fd = socket non bloquant (fermé par l'appelant),
 *              target = adresse de la cible, port = port à tester,
 *              timeout_ms = délai maximum en millisecondes.
 * Retour : PORT_OPEN, PORT_CLOSED ou PORT_FILTERED.
 */
port_state_t connect_with_timeout(int fd, const struct sockaddr_in *target,
                                  int port, int timeout_ms)
{
    struct sockaddr_in address = *target;

    address.sin_port = htons((uint16_t)port);
    /* connect lance la poignée de main TCP (envoi du SYN). En mode non
     * bloquant : 0 = connecté tout de suite, -1 avec EINPROGRESS = en
     * cours, -1 avec un autre errno (ECONNREFUSED...) = échec immédiat. */
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0) {
        return PORT_OPEN;
    }
    if (errno == EINPROGRESS) {
        return finish_pending_connection(fd, timeout_ms);
    }
    return state_from_error(errno);
}

/*
 * Attend des données sur un socket puis les lit.
 * Paramètres : fd = socket connecté, buffer/size = tampon de lecture.
 * Retour : nombre d'octets lus, 0 si le serveur a fermé, -1 si rien reçu.
 */
static ssize_t receive_data(int fd, char *buffer, size_t size)
{
    if (wait_for_fd(fd, 0, BANNER_WAIT_MS) != 1) {
        return -1;
    }
    /* recv lit au plus "size" octets ; renvoie le nombre d'octets lus,
     * 0 si le serveur a fermé la connexion, -1 en cas d'erreur. */
    return recv(fd, buffer, size, 0);
}

/*
 * Indique si un port fait partie des ports web connus (HTTP_PORTS).
 * Paramètre : port = numéro du port.
 * Retour : 1 si oui, 0 sinon.
 */
static int is_http_port(int port)
{
    for (size_t i = 0; i < HTTP_PORT_COUNT; i++) {
        if (HTTP_PORTS[i] == port) {
            return 1;
        }
    }
    return 0;
}

/*
 * Récupère la bannière d'un service sur un socket déjà connecté.
 * Paramètres : fd = socket connecté, port = port du service,
 *              banner/banner_size = tampon qui reçoit la bannière nettoyée
 *              (chaîne vide si rien n'est reçu).
 */
static void grab_banner(int fd, int port, char *banner, size_t banner_size)
{
    char raw[BANNER_SIZE];
    ssize_t received = receive_data(fd, raw, sizeof(raw) - 1);

    if (received < 0 && is_http_port(port)) {
        /* send envoie la requête HTTP ; MSG_NOSIGNAL évite que le programme
         * soit tué par SIGPIPE si le serveur a déjà fermé ; -1 si erreur. */
        if (send(fd, HTTP_REQUEST, strlen(HTTP_REQUEST), MSG_NOSIGNAL) >= 0) {
            received = receive_data(fd, raw, sizeof(raw) - 1);
        }
    }
    if (received > 0) {
        sanitize_banner(raw, (size_t)received, banner, banner_size);
    } else {
        banner[0] = '\0';
    }
}

/*
 * Teste un port de la cible et remplit son résultat (état et bannière).
 * Paramètres : target = adresse de la cible, timeout_ms = délai maximum,
 *              result = résultat à remplir (result->port doit être rempli).
 */
void scan_port(const struct sockaddr_in *target, int timeout_ms,
               port_result_t *result)
{
    int fd = create_nonblocking_socket();

    result->banner[0] = '\0';
    if (fd < 0) {
        result->state = PORT_FILTERED;
        return;
    }
    result->state = connect_with_timeout(fd, target, result->port, timeout_ms);
    if (result->state == PORT_OPEN) {
        grab_banner(fd, result->port, result->banner, sizeof(result->banner));
    }
    /* close ferme le socket dans tous les cas (ouvert, fermé ou filtré) ;
     * renvoie -1 en cas d'erreur, sans conséquence ici. */
    close(fd);
}

/*
 * Alloue le tableau des résultats, une case par port à tester.
 * Paramètres : ports = ports à tester, count = nombre de ports.
 * Retour : tableau alloué (à libérer avec free), ou NULL si plus de mémoire.
 */
port_result_t *create_results(const int *ports, size_t count)
{
    /* calloc met toute la mémoire à zéro : chaînes vides, scanned = 0. */
    port_result_t *results = calloc(count, sizeof(port_result_t));

    if (results == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        results[i].port = ports[i];
        results[i].state = PORT_FILTERED;
    }
    return results;
}

/*
 * Donne au thread appelant l'indice du prochain port à tester. L'indice
 * partagé est protégé par un verrou : deux threads ne peuvent jamais
 * recevoir le même port.
 * Paramètres : job = travail partagé, index = reçoit l'indice.
 * Retour : 1 si un port reste à tester, 0 si tout a été distribué.
 */
static int take_next_index(scan_job_t *job, size_t *index)
{
    int found = 0;

    /* pthread_mutex_lock attend que le verrou soit libre puis le prend ;
     * il ne peut pas échouer sur un verrou normal bien initialisé. */
    pthread_mutex_lock(&job->lock);
    if (job->next_index < job->count) {
        *index = job->next_index;
        job->next_index++;
        found = 1;
    }
    /* pthread_mutex_unlock rend le verrou aux autres threads. */
    pthread_mutex_unlock(&job->lock);
    return found;
}

/*
 * Travail d'un thread : prend les ports un par un et les teste, jusqu'à ce
 * qu'il n'en reste plus ou que l'utilisateur appuie sur Ctrl+C. Chaque
 * résultat est écrit dans sa propre case : aucune autre écriture partagée.
 * Paramètre : argument = pointeur vers le scan_job_t partagé.
 * Retour : NULL (rien à renvoyer).
 */
static void *scan_worker(void *argument)
{
    scan_job_t *job = argument;
    size_t index = 0;

    while (!g_stop_requested && take_next_index(job, &index)) {
        scan_port(job->target, job->timeout_ms, &job->results[index]);
        /* Si Ctrl+C arrive pendant le test, le résultat est ignoré. */
        if (!g_stop_requested) {
            job->results[index].scanned = 1;
        }
    }
    return NULL;
}

/*
 * Lance les threads de scan.
 * Paramètres : job = travail partagé, threads = tableau des identifiants,
 *              thread_count = nombre de threads voulus.
 * Retour : nombre de threads réellement lancés.
 */
static int start_workers(scan_job_t *job, pthread_t *threads, int thread_count)
{
    int started = 0;

    while (started < thread_count) {
        /* pthread_create lance un thread qui exécute scan_worker ; renvoie
         * 0 si succès, sinon un code d'erreur (EAGAIN : pas assez de
         * ressources). */
        int error = pthread_create(&threads[started], NULL, scan_worker, job);

        if (error != 0) {
            fprintf(stderr, "Attention : seulement %d thread(s) créé(s) : %s\n",
                    started, strerror(error));
            break;
        }
        started++;
    }
    return started;
}

/*
 * Attend la fin de tous les threads lancés.
 * Paramètres : threads = identifiants, started = nombre de threads lancés.
 */
static void join_workers(pthread_t *threads, int started)
{
    for (int i = 0; i < started; i++) {
        /* pthread_join attend la fin du thread ; renvoie 0 si succès. */
        int error = pthread_join(threads[i], NULL);

        if (error != 0) {
            fprintf(stderr, "Attention : pthread_join : %s\n", strerror(error));
        }
    }
}

/*
 * Lance le travail avec un ou plusieurs threads. Avec un seul thread, le
 * thread principal fait le travail lui-même. Si aucun thread ne peut être
 * créé, le thread principal fait aussi tout le travail.
 * Paramètres : job = travail partagé, thread_count = nombre de threads.
 */
static void run_workers(scan_job_t *job, int thread_count)
{
    pthread_t threads[MAX_THREADS];
    int started;

    if (thread_count <= 1) {
        scan_worker(job);
        return;
    }
    if (thread_count > MAX_THREADS) {
        thread_count = MAX_THREADS;
    }
    started = start_workers(job, threads, thread_count);
    if (started == 0) {
        scan_worker(job);
    }
    join_workers(threads, started);
}

/*
 * Compte les ports testés jusqu'au bout.
 * Paramètres : results/count = résultats du scan.
 * Retour : nombre de cases dont "scanned" vaut 1.
 */
static size_t count_tested(const port_result_t *results, size_t count)
{
    size_t tested = 0;

    for (size_t i = 0; i < count; i++) {
        tested += (size_t)results[i].scanned;
    }
    return tested;
}

/*
 * Teste tous les ports, avec un ou plusieurs threads. S'arrête si
 * l'utilisateur appuie sur Ctrl+C.
 * Paramètres : target = adresse de la cible, results/count = résultats,
 *              timeout_ms = délai maximum par port,
 *              thread_count = nombre de threads (1 à MAX_THREADS),
 *              tested = reçoit le nombre de ports testés jusqu'au bout.
 * Retour : 0 si succès, -1 si le verrou ne peut pas être créé.
 */
int scan_ports(const struct sockaddr_in *target, port_result_t *results,
               size_t count, int timeout_ms, int thread_count, size_t *tested)
{
    scan_job_t job;

    job.target = target;
    job.results = results;
    job.count = count;
    job.timeout_ms = timeout_ms;
    job.next_index = 0;
    /* pthread_mutex_init prépare le verrou ; renvoie 0 si succès. */
    if (pthread_mutex_init(&job.lock, NULL) != 0) {
        return -1;
    }
    run_workers(&job, thread_count);
    /* pthread_mutex_destroy libère le verrou (plus aucun thread actif). */
    pthread_mutex_destroy(&job.lock);
    *tested = count_tested(results, count);
    return 0;
}

/*
 * Cherche le nom du service associé à un port TCP.
 * Paramètres : port = numéro du port, service/size = tampon de sortie
 *              ("inconnu" si le port n'est pas dans /etc/services).
 */
static void lookup_service_name(int port, char *service, size_t size)
{
    /* getservbyport cherche le port (en ordre réseau, d'où htons) dans la
     * base des services ; renvoie NULL s'il est inconnu. */
    struct servent *entry = getservbyport(htons((uint16_t)port), "tcp");

    if (entry == NULL || entry->s_name == NULL) {
        snprintf(service, size, "inconnu");
        return;
    }
    snprintf(service, size, "%s", entry->s_name);
    to_lowercase(service);
}

/*
 * Remplit le nom du service de chaque port ouvert. Appelée après le scan,
 * par le seul thread principal (getservbyport n'est pas thread-safe).
 * Paramètres : results/count = résultats du scan.
 */
void lookup_service_names(port_result_t *results, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (results[i].scanned && results[i].state == PORT_OPEN) {
            lookup_service_name(results[i].port, results[i].service,
                                sizeof(results[i].service));
        }
    }
    /* endservent ferme la base des services ouverte par getservbyport. */
    endservent();
}
