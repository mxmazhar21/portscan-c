/*
 * Fichier : report.c
 * Ce fichier sert à présenter les résultats du scan : tableau aligné dans
 * le terminal, rapport texte (même contenu + date) et rapport HTML. Chaque
 * fonction d'écriture renvoie -1 si fprintf échoue, pour que le programme
 * puisse signaler une erreur d'écriture.
 */
#include "report.h"

#include <string.h>

/* Largeur (en caractères) de la colonne PORT, assez pour "65535/tcp ". */
#define PORT_COLUMN_WIDTH 10
/* Largeur de la colonne ÉTAT ("filtré" + espaces). */
#define STATE_COLUMN_WIDTH 9
/* Largeur de la colonne SERVICE. */
#define SERVICE_COLUMN_WIDTH 12
/* Taille du tampon qui reçoit "65535/tcp". */
#define PORT_TEXT_SIZE 16
/* Taille d'un tampon de texte échappé pour le HTML. */
#define HTML_BUFFER_SIZE (BANNER_SIZE * HTML_ESCAPE_FACTOR)

/*
 * Donne le libellé français d'un état de port.
 * Paramètre : state = état du port.
 * Retour : "ouvert", "fermé" ou "filtré".
 */
static const char *state_label(port_state_t state)
{
    switch (state) {
    case PORT_OPEN:
        return "ouvert";
    case PORT_CLOSED:
        return "fermé";
    default:
        return "filtré";
    }
}

/*
 * Écrit un texte suivi d'espaces pour remplir une colonne de largeur fixe.
 * La largeur de printf compte des octets : on l'augmente des octets en trop
 * des lettres accentuées (UTF-8) pour garder les colonnes alignées. Un
 * espace est toujours ajouté à la fin pour séparer les colonnes.
 * Paramètres : out = fichier de sortie, text = texte, width = largeur.
 * Retour : 0 si succès, -1 si l'écriture échoue.
 */
static int print_column(FILE *out, const char *text, int width)
{
    int byte_width = width - 1 + utf8_extra_bytes(text);

    return (fprintf(out, "%-*s ", byte_width, text) < 0) ? -1 : 0;
}

/*
 * Compte les ports ouverts, fermés et filtrés parmi les ports testés.
 * Paramètres : report = scan, open/closed/filtered = compteurs à remplir.
 */
static void count_states(const scan_report_t *report, size_t *open,
                         size_t *closed, size_t *filtered)
{
    *open = 0;
    *closed = 0;
    *filtered = 0;
    for (size_t i = 0; i < report->count; i++) {
        const port_result_t *result = &report->results[i];

        if (!result->scanned) {
            continue;
        }
        if (result->state == PORT_OPEN) {
            (*open)++;
        } else if (result->state == PORT_CLOSED) {
            (*closed)++;
        } else {
            (*filtered)++;
        }
    }
}

/*
 * Écrit la ligne de titre et l'en-tête des colonnes.
 * Paramètres : out = fichier de sortie, report = scan.
 * Retour : 0 si succès, -1 si l'écriture échoue.
 */
static int print_header(FILE *out, const scan_report_t *report)
{
    if (fprintf(out, "PortScan-C - cible : %s (%s) - ports : %s - délai : %d ms\n",
                report->target, report->ip_text, report->port_spec,
                report->timeout_ms) < 0) {
        return -1;
    }
    if (print_column(out, "PORT", PORT_COLUMN_WIDTH) < 0
        || print_column(out, "ÉTAT", STATE_COLUMN_WIDTH) < 0
        || print_column(out, "SERVICE", SERVICE_COLUMN_WIDTH) < 0) {
        return -1;
    }
    return (fprintf(out, "BANNIÈRE\n") < 0) ? -1 : 0;
}

/*
 * Écrit la ligne d'un port dans le tableau.
 * Paramètres : out = fichier de sortie, result = résultat du port.
 * Retour : 0 si succès, -1 si l'écriture échoue.
 */
static int print_result_line(FILE *out, const port_result_t *result)
{
    char port_text[PORT_TEXT_SIZE];
    const char *service = (result->service[0] != '\0') ? result->service : "-";

    snprintf(port_text, sizeof(port_text), "%d/tcp", result->port);
    if (print_column(out, port_text, PORT_COLUMN_WIDTH) < 0
        || print_column(out, state_label(result->state), STATE_COLUMN_WIDTH) < 0) {
        return -1;
    }
    /* Sans bannière, on évite d'écrire des espaces inutiles en fin de ligne. */
    if (result->banner[0] == '\0') {
        return (fprintf(out, "%s\n", service) < 0) ? -1 : 0;
    }
    if (print_column(out, service, SERVICE_COLUMN_WIDTH) < 0) {
        return -1;
    }
    return (fprintf(out, "%s\n", result->banner) < 0) ? -1 : 0;
}

/*
 * Écrit une ligne par port testé (seulement les ports ouverts, sauf en
 * mode verbeux où tous les ports testés sont affichés).
 * Paramètres : out = fichier de sortie, report = scan.
 * Retour : 0 si succès, -1 si l'écriture échoue.
 */
static int print_result_lines(FILE *out, const scan_report_t *report)
{
    size_t shown = 0;

    for (size_t i = 0; i < report->count; i++) {
        const port_result_t *result = &report->results[i];

        if (!result->scanned
            || (!report->verbose && result->state != PORT_OPEN)) {
            continue;
        }
        if (print_result_line(out, result) < 0) {
            return -1;
        }
        shown++;
    }
    if (shown == 0 && fprintf(out, "(aucun port ouvert)\n") < 0) {
        return -1;
    }
    return 0;
}

/*
 * Écrit la ligne de résumé (compteurs, durée, interruption éventuelle).
 * Paramètres : out = fichier de sortie, report = scan.
 * Retour : 0 si succès, -1 si l'écriture échoue.
 */
static int print_summary(FILE *out, const scan_report_t *report)
{
    size_t open;
    size_t closed;
    size_t filtered;

    count_states(report, &open, &closed, &filtered);
    if (fprintf(out, "----\nRésumé : %zu ouverts, %zu fermés, %zu filtrés"
                " - durée : %.2f s", open, closed, filtered,
                report->duration) < 0) {
        return -1;
    }
    if (report->interrupted
        && fprintf(out, " - scan interrompu (%zu/%zu ports testés)",
                   report->tested, report->count) < 0) {
        return -1;
    }
    return (fprintf(out, "\n") < 0) ? -1 : 0;
}

/*
 * Affiche le résultat complet du scan au format du terminal.
 * Paramètres : out = fichier de sortie (stdout ou rapport texte),
 *              report = scan.
 * Retour : 0 si succès, -1 si l'écriture échoue.
 */
int print_report(FILE *out, const scan_report_t *report)
{
    if (print_header(out, report) < 0
        || print_result_lines(out, report) < 0
        || print_summary(out, report) < 0) {
        return -1;
    }
    return 0;
}

/*
 * Écrit le rapport texte : la date du scan puis le même contenu que le
 * terminal.
 * Paramètres : file = fichier ouvert en écriture, report = scan.
 * Retour : 0 si succès, -1 si l'écriture échoue.
 */
static int write_text_report(FILE *file, const scan_report_t *report)
{
    if (fprintf(file, "Date du scan : %s\n", report->date) < 0) {
        return -1;
    }
    return print_report(file, report);
}

/*
 * Écrit le début de la page HTML : doctype, encodage, titre et style CSS.
 * Paramètre : file = fichier ouvert en écriture.
 * Retour : 0 si succès, -1 si l'écriture échoue.
 */
static int write_html_head(FILE *file)
{
    int status = fprintf(file,
        "<!DOCTYPE html>\n<html lang=\"fr\">\n<head>\n"
        "<meta charset=\"UTF-8\">\n<title>Rapport PortScan-C</title>\n"
        "<style>\n"
        "body { font-family: Arial, sans-serif; margin: 2em; color: #222; }\n"
        "table { border-collapse: collapse; }\n"
        "th, td { border: 1px solid #999; padding: 4px 10px; text-align: left; }\n"
        "th { background: #ddd; }\n"
        "code { font-family: monospace; }\n"
        ".warning { color: #a00; }\n"
        "</style>\n</head>\n<body>\n");

    return (status < 0) ? -1 : 0;
}

/*
 * Écrit le titre et les informations générales du scan en HTML.
 * Paramètres : file = fichier ouvert en écriture, report = scan.
 * Retour : 0 si succès, -1 si l'écriture échoue.
 */
static int write_html_info(FILE *file, const scan_report_t *report)
{
    char target[HTML_BUFFER_SIZE];
    int status;

    /* La cible vient de l'utilisateur : on l'échappe aussi. La liste de
     * ports a déjà été validée (chiffres, virgules et tirets). */
    html_escape(report->target, target, sizeof(target));
    status = fprintf(file,
        "<h1>Rapport PortScan-C</h1>\n<ul>\n"
        "<li>Date du scan : %s</li>\n<li>Cible : %s (%s)</li>\n"
        "<li>Ports demandés : %s</li>\n<li>Délai par port : %d ms</li>\n"
        "</ul>\n", report->date, target, report->ip_text,
        report->port_spec, report->timeout_ms);
    return (status < 0) ? -1 : 0;
}

/*
 * Écrit la ligne HTML d'un port ouvert. La bannière vient du réseau : elle
 * passe obligatoirement par html_escape.
 * Paramètres : file = fichier ouvert en écriture, result = port ouvert.
 * Retour : 0 si succès, -1 si l'écriture échoue.
 */
static int write_html_row(FILE *file, const port_result_t *result)
{
    char banner[HTML_BUFFER_SIZE];
    char service[HTML_BUFFER_SIZE];

    html_escape(result->banner, banner, sizeof(banner));
    html_escape(result->service, service, sizeof(service));
    if (fprintf(file, "<tr><td>%d/tcp</td><td>%s</td><td><code>%s</code></td></tr>\n",
                result->port, service, banner) < 0) {
        return -1;
    }
    return 0;
}

/*
 * Écrit le tableau HTML des ports ouverts.
 * Paramètres : file = fichier ouvert en écriture, report = scan.
 * Retour : 0 si succès, -1 si l'écriture échoue.
 */
static int write_html_table(FILE *file, const scan_report_t *report)
{
    if (fprintf(file, "<h2>Ports ouverts</h2>\n<table>\n"
                "<tr><th>Port</th><th>Service</th><th>Bannière</th></tr>\n") < 0) {
        return -1;
    }
    for (size_t i = 0; i < report->count; i++) {
        const port_result_t *result = &report->results[i];

        if (result->scanned && result->state == PORT_OPEN
            && write_html_row(file, result) < 0) {
            return -1;
        }
    }
    return (fprintf(file, "</table>\n") < 0) ? -1 : 0;
}

/*
 * Écrit le résumé, l'avertissement légal et la fin de la page HTML.
 * Paramètres : file = fichier ouvert en écriture, report = scan.
 * Retour : 0 si succès, -1 si l'écriture échoue.
 */
static int write_html_summary(FILE *file, const scan_report_t *report)
{
    size_t open;
    size_t closed;
    size_t filtered;

    count_states(report, &open, &closed, &filtered);
    if (fprintf(file, "<h2>Résumé</h2>\n<p>%zu ouverts, %zu fermés, %zu filtrés"
                " - durée : %.2f s</p>\n", open, closed, filtered,
                report->duration) < 0) {
        return -1;
    }
    if (report->interrupted
        && fprintf(file, "<p class=\"warning\">Scan interrompu (%zu/%zu ports"
                   " testés)</p>\n", report->tested, report->count) < 0) {
        return -1;
    }
    if (fprintf(file, "<p class=\"warning\">À utiliser uniquement sur des machines"
                " qui vous appartiennent ou avec autorisation écrite.</p>\n"
                "</body>\n</html>\n") < 0) {
        return -1;
    }
    return 0;
}

/*
 * Écrit le rapport HTML complet.
 * Paramètres : file = fichier ouvert en écriture, report = scan.
 * Retour : 0 si succès, -1 si l'écriture échoue.
 */
static int write_html_report(FILE *file, const scan_report_t *report)
{
    if (write_html_head(file) < 0
        || write_html_info(file, report) < 0
        || write_html_table(file, report) < 0
        || write_html_summary(file, report) < 0) {
        return -1;
    }
    return 0;
}

/*
 * Indique si un nom de fichier se termine par ".html".
 * Paramètre : path = nom du fichier.
 * Retour : 1 si oui, 0 sinon.
 */
static int has_html_extension(const char *path)
{
    const char *extension = ".html";
    size_t path_length = strlen(path);
    size_t extension_length = strlen(extension);

    if (path_length < extension_length) {
        return 0;
    }
    return strcmp(path + path_length - extension_length, extension) == 0;
}

/*
 * Écrit le rapport dans un fichier : HTML si le nom finit par ".html",
 * texte sinon.
 * Paramètres : path = nom du fichier, report = scan.
 * Retour : 0 si succès, -1 si l'ouverture, l'écriture ou la fermeture
 *          échoue (errno indique alors la cause).
 */
int write_report_file(const char *path, const scan_report_t *report)
{
    FILE *file;
    int status;

    /* fopen crée (ou vide) le fichier ; NULL en cas d'échec (droits
     * insuffisants, dossier inexistant...). */
    file = fopen(path, "w");
    if (file == NULL) {
        return -1;
    }
    if (has_html_extension(path)) {
        status = write_html_report(file, report);
    } else {
        status = write_text_report(file, report);
    }
    /* fclose écrit ce qui reste dans le tampon puis ferme le fichier ;
     * renvoie EOF si cette dernière écriture échoue. */
    if (fclose(file) != 0) {
        status = -1;
    }
    return status;
}
