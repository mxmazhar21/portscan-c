/*
 * Fichier : main.c
 * Ce fichier sert à lire les arguments de la ligne de commande (getopt),
 * puis à enchaîner les étapes : décodage des ports, résolution de la cible,
 * scan, affichage des résultats et écriture du rapport.
 */
#include "ports.h"
#include "report.h"
#include "scanner.h"
#include "utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Code de sortie : tout s'est bien passé. */
#define EXIT_CODE_OK 0
/* Code de sortie : mauvaise utilisation (option ou valeur invalide). */
#define EXIT_CODE_USAGE 1
/* Code de sortie : le nom de la cible ne peut pas être résolu. */
#define EXIT_CODE_RESOLVE 2
/* Code de sortie : erreur système (mémoire, fichier de rapport). */
#define EXIT_CODE_SYSTEM 3
/* Code de sortie : scan interrompu par Ctrl+C (128 + numéro de SIGINT). */
#define EXIT_CODE_INTERRUPTED 130

/* Délai par port utilisé si -t n'est pas donné (millisecondes). */
#define DEFAULT_TIMEOUT_MS 500
/* Plus petit délai accepté pour -t (millisecondes). */
#define MIN_TIMEOUT_MS 50
/* Plus grand délai accepté pour -t (millisecondes). */
#define MAX_TIMEOUT_MS 10000
/* Nombre de threads utilisé si -j n'est pas donné. */
#define DEFAULT_THREADS 1
/* Plus petit nombre de threads accepté pour -j (le maximum, MAX_THREADS,
 * est défini dans scanner.h). */
#define MIN_THREADS 1

/* Options choisies par l'utilisateur. */
typedef struct {
    const char *target;      /* cible (obligatoire) */
    const char *port_spec;   /* texte de l'option -p */
    const char *output_path; /* fichier de rapport, ou NULL */
    long timeout_ms;         /* délai par port */
    long thread_count;       /* nombre de threads (option -j) */
    int verbose;             /* option -v */
} options_t;

/* Résultat de la lecture des options. */
typedef enum {
    OPTIONS_OK,    /* on peut lancer le scan */
    OPTIONS_HELP,  /* -h : afficher l'aide et quitter avec le code 0 */
    OPTIONS_ERROR  /* erreur : afficher l'aide et quitter avec le code 1 */
} options_status_t;

/*
 * Affiche l'aide du programme.
 * Paramètres : out = stdout (option -h) ou stderr (erreur),
 *              program = nom du programme (argv[0]).
 */
static void print_usage(FILE *out, const char *program)
{
    fprintf(out,
        "Usage : %s <cible> [-p ports] [-t timeout_ms] [-o fichier] [-j threads]"
        " [-v] [-h]\n"
        "  <cible>  adresse IPv4 ou nom d'hôte à scanner\n"
        "  -p       ports : 80, 1-1024, 22,80,443 ou 1-100,443 (défaut : %s)\n"
        "  -t       délai par port en ms, de %d à %d (défaut : %d)\n"
        "  -o       rapport : HTML si le nom finit par .html, texte sinon\n"
        "  -j       nombre de threads, de %d à %d (défaut : %d)\n"
        "  -v       affiche aussi les ports fermés et filtrés\n"
        "  -h       affiche cette aide\n",
        program, DEFAULT_PORT_SPEC, MIN_TIMEOUT_MS, MAX_TIMEOUT_MS,
        DEFAULT_TIMEOUT_MS, MIN_THREADS, MAX_THREADS, DEFAULT_THREADS);
}

/*
 * Gestionnaire du signal SIGINT (Ctrl+C) : il lève seulement le drapeau
 * d'arrêt, le scan s'arrête ensuite proprement.
 * Paramètre : signal_number = numéro du signal (non utilisé).
 */
static void handle_sigint(int signal_number)
{
    (void)signal_number;
    g_stop_requested = 1;
}

/*
 * Installe le gestionnaire de SIGINT avec sigaction.
 * Retour : 0 si succès, -1 en cas d'erreur.
 */
static int install_sigint_handler(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_sigint;
    /* Pas de SA_RESTART : select est interrompu (EINTR) par Ctrl+C. */
    action.sa_flags = 0;
    /* sigemptyset vide la liste des signaux bloqués pendant le gestionnaire. */
    if (sigemptyset(&action.sa_mask) != 0) {
        return -1;
    }
    /* sigaction installe le gestionnaire ; renvoie -1 en cas d'erreur. */
    return sigaction(SIGINT, &action, NULL);
}

/*
 * Convertit la valeur numérique d'une option et vérifie ses bornes.
 * Paramètres : text = valeur tapée, min/max = bornes, value = résultat,
 *              name = nom de l'option pour le message d'erreur.
 * Retour : OPTIONS_OK ou OPTIONS_ERROR.
 */
static options_status_t read_number_option(const char *text, long min,
                                           long max, long *value,
                                           const char *name)
{
    if (parse_long_in_range(text, min, max, value) != 0) {
        fprintf(stderr, "Erreur : %s invalide \"%s\" (entier de %ld à %ld"
                " attendu).\n", name, text, min, max);
        return OPTIONS_ERROR;
    }
    return OPTIONS_OK;
}

/*
 * Applique une option renvoyée par getopt.
 * Paramètres : option = lettre de l'option, argument = sa valeur (optarg),
 *              options = options à mettre à jour.
 * Retour : OPTIONS_OK, OPTIONS_HELP ou OPTIONS_ERROR.
 */
static options_status_t apply_option(int option, const char *argument,
                                     options_t *options)
{
    switch (option) {
    case 'p':
        options->port_spec = argument;
        return OPTIONS_OK;
    case 't':
        return read_number_option(argument, MIN_TIMEOUT_MS, MAX_TIMEOUT_MS,
                                  &options->timeout_ms, "délai (-t)");
    case 'o':
        options->output_path = argument;
        return OPTIONS_OK;
    case 'j':
        return read_number_option(argument, MIN_THREADS, MAX_THREADS,
                                  &options->thread_count, "nombre de threads (-j)");
    case 'v':
        options->verbose = 1;
        return OPTIONS_OK;
    case 'h':
        return OPTIONS_HELP;
    case ':':
        fprintf(stderr, "Erreur : l'option -%c demande une valeur.\n", optopt);
        return OPTIONS_ERROR;
    default:
        fprintf(stderr, "Erreur : option inconnue -%c.\n", optopt);
        return OPTIONS_ERROR;
    }
}

/*
 * Donne aux options leur valeur par défaut.
 * Paramètre : options = options à initialiser.
 */
static void set_default_options(options_t *options)
{
    options->target = NULL;
    options->port_spec = DEFAULT_PORT_SPEC;
    options->output_path = NULL;
    options->timeout_ms = DEFAULT_TIMEOUT_MS;
    options->thread_count = DEFAULT_THREADS;
    options->verbose = 0;
}

/*
 * Enregistre la cible (argument qui n'est pas une option).
 * Paramètres : argument = texte de la cible, options = options.
 * Retour : OPTIONS_OK, ou OPTIONS_ERROR si une cible a déjà été donnée.
 */
static options_status_t set_target(const char *argument, options_t *options)
{
    if (options->target != NULL) {
        fprintf(stderr, "Erreur : une seule cible est acceptée.\n");
        return OPTIONS_ERROR;
    }
    options->target = argument;
    return OPTIONS_OK;
}

/*
 * Lit toutes les options de la ligne de commande avec getopt. Le getopt
 * POSIX s'arrête au premier argument qui n'est pas une option : on le prend
 * comme cible, on passe à l'argument suivant et on continue la lecture.
 * Paramètres : argc/argv = arguments du programme, options = résultat.
 * Retour : OPTIONS_OK, OPTIONS_HELP ou OPTIONS_ERROR.
 */
static options_status_t parse_options(int argc, char *argv[],
                                      options_t *options)
{
    options_status_t status = OPTIONS_OK;

    set_default_options(options);
    opterr = 0; /* getopt n'affiche rien : nos messages sont en français */
    while (status == OPTIONS_OK && optind < argc) {
        /* Le ':' au début permet de distinguer "valeur manquante" (':')
         * de "option inconnue" ('?'). */
        int option = getopt(argc, argv, ":p:t:o:j:vh");

        if (option == -1) {
            if (optind >= argc) {
                break;
            }
            status = set_target(argv[optind], options);
            optind++;
        } else {
            status = apply_option(option, optarg, options);
        }
    }
    if (status == OPTIONS_OK && options->target == NULL) {
        fprintf(stderr, "Erreur : la cible est obligatoire.\n");
        status = OPTIONS_ERROR;
    }
    return status;
}

/*
 * Affiche les résultats dans le terminal et écrit le rapport si -o est
 * donné.
 * Paramètres : report = scan, output_path = fichier de rapport ou NULL.
 * Retour : EXIT_CODE_OK ou EXIT_CODE_SYSTEM.
 */
static int output_results(const scan_report_t *report, const char *output_path)
{
    if (print_report(stdout, report) < 0) {
        fprintf(stderr, "Erreur : écriture impossible sur la sortie standard.\n");
        return EXIT_CODE_SYSTEM;
    }
    if (output_path == NULL) {
        return EXIT_CODE_OK;
    }
    if (write_report_file(output_path, report) < 0) {
        fprintf(stderr, "Erreur : impossible d'écrire le rapport \"%s\" : %s\n",
                output_path, strerror(errno));
        return EXIT_CODE_SYSTEM;
    }
    fprintf(stderr, "Rapport écrit dans %s\n", output_path);
    return EXIT_CODE_OK;
}

/*
 * Prépare la structure qui décrit le scan pour l'affichage.
 * Paramètres : report = structure à remplir, options = options,
 *              ip_text = IP résolue, results/count = résultats.
 */
static void fill_report(scan_report_t *report, const options_t *options,
                        const char *ip_text, const port_result_t *results,
                        size_t count)
{
    memset(report, 0, sizeof(*report));
    report->target = options->target;
    report->ip_text = ip_text;
    report->port_spec = options->port_spec;
    report->timeout_ms = (int)options->timeout_ms;
    report->verbose = options->verbose;
    report->results = results;
    report->count = count;
    format_current_date(report->date, sizeof(report->date));
}

/*
 * Teste tous les ports en mesurant la durée, puis cherche les services.
 * Paramètres : options = options, target = adresse de la cible,
 *              results = résultats à remplir, report = scan à compléter
 *              (durée, nombre de ports testés, interruption).
 * Retour : 0 si succès, -1 si le scan n'a pas pu démarrer.
 */
static int perform_scan(const options_t *options,
                        const struct sockaddr_in *target,
                        port_result_t *results, scan_report_t *report)
{
    struct timespec start;
    struct timespec end;

    fprintf(stderr, "Scan de %zu port(s) en cours avec %ld thread(s)...\n",
            report->count, options->thread_count);
    get_monotonic_time(&start);
    if (scan_ports(target, results, report->count, (int)options->timeout_ms,
                   (int)options->thread_count, &report->tested) != 0) {
        fprintf(stderr, "Erreur : création du verrou des threads impossible.\n");
        return -1;
    }
    get_monotonic_time(&end);
    report->duration = elapsed_seconds(&start, &end);
    report->interrupted = (g_stop_requested != 0);
    lookup_service_names(results, report->count);
    return 0;
}

/*
 * Alloue les résultats, lance le scan, affiche les résultats puis libère
 * la mémoire.
 * Paramètres : options = options, list = ports à tester,
 *              target = adresse de la cible, ip_text = IP en texte.
 * Retour : code de sortie du programme.
 */
static int run_scan(const options_t *options, const port_list_t *list,
                    const struct sockaddr_in *target, const char *ip_text)
{
    port_result_t *results = create_results(list->ports, list->count);
    scan_report_t report;
    int code;

    if (results == NULL) {
        fprintf(stderr, "Erreur : mémoire insuffisante.\n");
        return EXIT_CODE_SYSTEM;
    }
    fill_report(&report, options, ip_text, results, list->count);
    if (perform_scan(options, target, results, &report) != 0) {
        free(results);
        return EXIT_CODE_SYSTEM;
    }
    code = output_results(&report, options->output_path);
    free(results);
    if (code == EXIT_CODE_OK && report.interrupted) {
        code = EXIT_CODE_INTERRUPTED;
    }
    return code;
}

/*
 * Résout la cible puis lance le scan.
 * Paramètres : options = options, list = ports à tester.
 * Retour : code de sortie du programme.
 */
static int run_with_target(const options_t *options, const port_list_t *list)
{
    struct sockaddr_in target;
    char ip_text[INET_ADDRSTRLEN];
    int status = resolve_target(options->target, &target, ip_text,
                                sizeof(ip_text));

    if (status != 0) {
        fprintf(stderr, "Erreur : impossible de résoudre \"%s\" : %s\n",
                options->target, gai_strerror(status));
        return EXIT_CODE_RESOLVE;
    }
    fprintf(stderr, "Cible %s résolue en %s\n", options->target, ip_text);
    if (install_sigint_handler() != 0) {
        fprintf(stderr, "Erreur : impossible d'installer le gestionnaire"
                " de Ctrl+C : %s\n", strerror(errno));
        return EXIT_CODE_SYSTEM;
    }
    return run_scan(options, list, &target, ip_text);
}

/*
 * Décode la liste de ports, lance le reste du programme puis libère la
 * liste.
 * Paramètre : options = options.
 * Retour : code de sortie du programme.
 */
static int run_with_ports(const options_t *options)
{
    port_list_t list;
    port_error_t error = parse_port_list(options->port_spec, &list);
    int code;

    if (error != PORTS_OK) {
        fprintf(stderr, "Erreur : liste de ports \"%s\" invalide : %s.\n",
                options->port_spec, port_error_message(error));
        return (error == PORTS_ERROR_MEMORY) ? EXIT_CODE_SYSTEM : EXIT_CODE_USAGE;
    }
    code = run_with_target(options, &list);
    free_port_list(&list);
    return code;
}

/*
 * Point d'entrée : avertissement légal, lecture des options puis scan.
 * Paramètres : argc/argv = arguments de la ligne de commande.
 * Retour : 0 succès, 1 utilisation, 2 résolution, 3 système, 130 Ctrl+C.
 */
int main(int argc, char *argv[])
{
    options_t options;
    options_status_t status;

    fprintf(stderr, "ATTENTION : à utiliser uniquement sur des machines qui"
            " vous appartiennent ou avec autorisation écrite.\n");
    status = parse_options(argc, argv, &options);
    if (status == OPTIONS_HELP) {
        print_usage(stdout, argv[0]);
        return EXIT_CODE_OK;
    }
    if (status == OPTIONS_ERROR) {
        print_usage(stderr, argv[0]);
        return EXIT_CODE_USAGE;
    }
    return run_with_ports(&options);
}
