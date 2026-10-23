/*
 * Fichier : utils.h
 * Ce fichier sert à déclarer les petites fonctions utilitaires du projet :
 * conversion de texte en nombre, nettoyage des bannières reçues du réseau,
 * échappement HTML, mesure du temps et mise en forme de la date.
 */
#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <time.h>

/* Un caractère échappé en HTML prend au plus 6 caractères ("&quot;"). */
#define HTML_ESCAPE_FACTOR 6

/* Taille du tampon qui reçoit la date du scan ("2026-10-23 14:05:00"). */
#define DATE_SIZE 32

int parse_long_in_range(const char *text, long min, long max, long *value);
void sanitize_banner(const char *raw, size_t raw_length,
                     char *output, size_t output_size);
size_t html_escape(const char *input, char *output, size_t output_size);
void to_lowercase(char *text);
int utf8_extra_bytes(const char *text);
void get_monotonic_time(struct timespec *now);
double elapsed_seconds(const struct timespec *start,
                       const struct timespec *end);
void format_current_date(char *output, size_t output_size);

#endif
