/*
 * Fichier : utils.c
 * Ce fichier sert à regrouper les fonctions utilitaires qui ne dépendent
 * pas du réseau : conversion sûre de texte en nombre (strtol), nettoyage
 * d'une bannière, échappement HTML, mesure du temps et date du scan.
 */
#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Convertit un texte en nombre entier et vérifie qu'il est dans [min, max].
 * Paramètres : text = texte à convertir, min/max = bornes autorisées,
 *              value = reçoit le nombre si tout est correct.
 * Retour : 0 si la conversion est correcte, -1 sinon (texte vide, signe,
 *          caractère en trop, nombre trop grand ou hors bornes).
 */
int parse_long_in_range(const char *text, long min, long max, long *value)
{
    char *end = NULL;
    long number;

    /* strtol accepterait des espaces et un signe : on exige un chiffre. */
    if (text == NULL || !isdigit((unsigned char)text[0])) {
        return -1;
    }
    errno = 0;
    /* strtol convertit en base 10 ; elle met errno à ERANGE si le nombre
     * dépasse la capacité d'un long, et "end" pointe après le dernier
     * chiffre lu. */
    number = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0') {
        return -1;
    }
    if (number < min || number > max) {
        return -1;
    }
    *value = number;
    return 0;
}

/*
 * Nettoie une bannière reçue du réseau : garde seulement la première ligne,
 * remplace chaque caractère non imprimable par un point et termine la
 * chaîne par un zéro.
 * Paramètres : raw = octets reçus (pas forcément terminés par un zéro),
 *              raw_length = nombre d'octets reçus,
 *              output/output_size = tampon de sortie et sa taille.
 */
void sanitize_banner(const char *raw, size_t raw_length,
                     char *output, size_t output_size)
{
    size_t written = 0;

    if (output == NULL || output_size == 0) {
        return;
    }
    for (size_t i = 0; i < raw_length && written < output_size - 1; i++) {
        unsigned char c = (unsigned char)raw[i];

        if (c == '\r' || c == '\n') {
            break;
        }
        output[written] = isprint(c) ? (char)c : '.';
        written++;
    }
    output[written] = '\0';
}

/*
 * Donne l'entité HTML qui remplace un caractère spécial.
 * Paramètre : c = caractère à tester.
 * Retour : l'entité ("&lt;"...) ou NULL si le caractère n'est pas spécial.
 */
static const char *html_entity(char c)
{
    switch (c) {
    case '&':
        return "&amp;";
    case '<':
        return "&lt;";
    case '>':
        return "&gt;";
    case '"':
        return "&quot;";
    case '\'':
        return "&#39;";
    default:
        return NULL;
    }
}

/*
 * Échappe un texte pour l'insérer sans danger dans une page HTML
 * (& < > " ' sont remplacés par leur entité). Si le tampon est trop petit,
 * le texte est coupé proprement, jamais au milieu d'une entité.
 * Paramètres : input = texte d'origine, output/output_size = tampon de sortie.
 * Retour : nombre de caractères écrits (sans le zéro final).
 */
size_t html_escape(const char *input, char *output, size_t output_size)
{
    size_t used = 0;

    if (output == NULL || output_size == 0) {
        return 0;
    }
    for (size_t i = 0; input != NULL && input[i] != '\0'; i++) {
        const char *entity = html_entity(input[i]);
        size_t piece_length = (entity != NULL) ? strlen(entity) : 1;

        if (used + piece_length >= output_size) {
            break;
        }
        if (entity != NULL) {
            memcpy(output + used, entity, piece_length);
        } else {
            output[used] = input[i];
        }
        used += piece_length;
    }
    output[used] = '\0';
    return used;
}

/*
 * Met toutes les lettres d'un texte en minuscules (modification sur place).
 * Paramètre : text = chaîne terminée par un zéro.
 */
void to_lowercase(char *text)
{
    for (size_t i = 0; text[i] != '\0'; i++) {
        text[i] = (char)tolower((unsigned char)text[i]);
    }
}

/*
 * Compte les octets "en trop" d'un texte UTF-8 : une lettre accentuée
 * comme "é" occupe 2 octets mais une seule colonne à l'écran.
 * Paramètre : text = chaîne UTF-8.
 * Retour : nombre d'octets de continuation (de la forme 10xxxxxx).
 */
int utf8_extra_bytes(const char *text)
{
    int extra = 0;

    for (size_t i = 0; text[i] != '\0'; i++) {
        if (((unsigned char)text[i] & 0xC0) == 0x80) {
            extra++;
        }
    }
    return extra;
}

/*
 * Lit l'horloge monotone (elle ne recule jamais, même si l'heure du système
 * change), utile pour mesurer une durée.
 * Paramètre : now = reçoit l'instant présent (mis à zéro en cas d'échec).
 */
void get_monotonic_time(struct timespec *now)
{
    /* clock_gettime renvoie 0 si succès, -1 en cas d'erreur. */
    if (clock_gettime(CLOCK_MONOTONIC, now) != 0) {
        now->tv_sec = 0;
        now->tv_nsec = 0;
    }
}

/*
 * Calcule la durée écoulée entre deux instants.
 * Paramètres : start = début, end = fin.
 * Retour : durée en secondes (avec décimales).
 */
double elapsed_seconds(const struct timespec *start,
                       const struct timespec *end)
{
    double seconds = (double)(end->tv_sec - start->tv_sec);

    return seconds + (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

/*
 * Écrit la date et l'heure locales actuelles au format "AAAA-MM-JJ HH:MM:SS".
 * Paramètres : output/output_size = tampon de sortie.
 */
void format_current_date(char *output, size_t output_size)
{
    time_t now = time(NULL);
    struct tm *local = NULL;

    if (now != (time_t)-1) {
        /* localtime découpe l'heure en champs (année, mois...) ; NULL si
         * la conversion échoue. */
        local = localtime(&now);
    }
    /* strftime renvoie 0 si le résultat ne tient pas dans le tampon. */
    if (local == NULL
        || strftime(output, output_size, "%Y-%m-%d %H:%M:%S", local) == 0) {
        snprintf(output, output_size, "date inconnue");
    }
}
