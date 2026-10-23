/*
 * Fichier : ports.c
 * Ce fichier sert à transformer le texte de l'option -p en un tableau de
 * ports trié et sans doublon. On utilise un tableau "seen" de 65536 cases :
 * seen[p] vaut 1 si le port p est demandé. Parcourir ce tableau dans
 * l'ordre donne directement des ports triés et sans doublon.
 */
#include "ports.h"
#include "utils.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*
 * Indique si un texte est non vide et ne contient que des chiffres.
 * Paramètre : text = chaîne à tester.
 * Retour : 1 si oui, 0 sinon.
 */
static int is_digit_string(const char *text)
{
    if (text[0] == '\0') {
        return 0;
    }
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (!isdigit((unsigned char)text[i])) {
            return 0;
        }
    }
    return 1;
}

/*
 * Convertit un numéro de port écrit en texte.
 * Paramètres : text = numéro en texte, port = reçoit le numéro.
 * Retour : PORTS_OK, PORTS_ERROR_SYNTAX ou PORTS_ERROR_RANGE.
 */
static port_error_t parse_port_number(const char *text, int *port)
{
    long value = 0;

    if (!is_digit_string(text)) {
        return PORTS_ERROR_SYNTAX;
    }
    if (parse_long_in_range(text, PORT_MIN, PORT_MAX, &value) != 0) {
        return PORTS_ERROR_RANGE;
    }
    *port = (int)value;
    return PORTS_OK;
}

/*
 * Décode un élément de la liste : un port seul ("80") ou une plage
 * ("1-1024"), et marque les ports correspondants dans "seen".
 * Paramètres : token = élément (modifiable), seen = tableau de marquage.
 * Retour : PORTS_OK ou un code d'erreur.
 */
static port_error_t parse_port_token(char *token, unsigned char *seen)
{
    char *dash = strchr(token, '-');
    int first = 0;
    int last = 0;
    port_error_t error;

    if (dash != NULL) {
        *dash = '\0';
    }
    error = parse_port_number(token, &first);
    if (error != PORTS_OK) {
        return error;
    }
    last = first;
    if (dash != NULL) {
        error = parse_port_number(dash + 1, &last);
        if (error != PORTS_OK) {
            return error;
        }
        if (first > last) {
            return PORTS_ERROR_ORDER;
        }
    }
    for (int port = first; port <= last; port++) {
        seen[port] = 1;
    }
    return PORTS_OK;
}

/*
 * Découpe le texte sur les virgules et décode chaque élément.
 * Paramètres : text = copie modifiable du texte, seen = tableau de marquage.
 * Retour : PORTS_OK ou le premier code d'erreur rencontré.
 */
static port_error_t mark_ports(char *text, unsigned char *seen)
{
    char *token = text;

    for (;;) {
        char *comma = strchr(token, ',');
        port_error_t error;

        if (comma != NULL) {
            *comma = '\0';
        }
        error = parse_port_token(token, seen);
        if (error != PORTS_OK) {
            return error;
        }
        if (comma == NULL) {
            return PORTS_OK;
        }
        token = comma + 1;
    }
}

/*
 * Construit le tableau final des ports à partir du tableau de marquage.
 * Paramètres : seen = tableau de marquage, list = liste à remplir.
 * Retour : PORTS_OK ou PORTS_ERROR_MEMORY.
 */
static port_error_t build_port_array(const unsigned char *seen,
                                     port_list_t *list)
{
    size_t count = 0;
    size_t index = 0;

    for (int port = PORT_MIN; port <= PORT_MAX; port++) {
        count += seen[port];
    }
    list->ports = malloc(count * sizeof(int));
    if (list->ports == NULL) {
        return PORTS_ERROR_MEMORY;
    }
    for (int port = PORT_MIN; port <= PORT_MAX; port++) {
        if (seen[port]) {
            list->ports[index] = port;
            index++;
        }
    }
    list->count = count;
    return PORTS_OK;
}

/*
 * Décode une liste de ports ("1-100,443,8000-8100").
 * Paramètres : text = texte de l'option -p, list = liste à remplir (à
 *              libérer avec free_port_list si le retour vaut PORTS_OK).
 * Retour : PORTS_OK ou un code d'erreur (la liste reste alors vide).
 */
port_error_t parse_port_list(const char *text, port_list_t *list)
{
    unsigned char *seen;
    char *copy;
    port_error_t error;

    list->ports = NULL;
    list->count = 0;
    if (text == NULL) {
        return PORTS_ERROR_SYNTAX;
    }
    seen = calloc(PORT_MAX + 1, sizeof(unsigned char));
    copy = strdup(text);
    if (seen == NULL || copy == NULL) {
        free(seen);
        free(copy);
        return PORTS_ERROR_MEMORY;
    }
    error = mark_ports(copy, seen);
    if (error == PORTS_OK) {
        error = build_port_array(seen, list);
    }
    free(seen);
    free(copy);
    return error;
}

/*
 * Libère le tableau de ports et remet la liste à zéro.
 * Paramètre : list = liste remplie par parse_port_list.
 */
void free_port_list(port_list_t *list)
{
    free(list->ports);
    list->ports = NULL;
    list->count = 0;
}

/*
 * Donne un message en français qui explique une erreur de décodage.
 * Paramètre : error = code renvoyé par parse_port_list.
 * Retour : message constant (ne pas libérer).
 */
const char *port_error_message(port_error_t error)
{
    switch (error) {
    case PORTS_OK:
        return "aucune erreur";
    case PORTS_ERROR_SYNTAX:
        return "format attendu : 80, 1-1024, 22,80,443 ou un mélange";
    case PORTS_ERROR_RANGE:
        return "chaque port doit être compris entre 1 et 65535";
    case PORTS_ERROR_ORDER:
        return "plage inversée (le début doit être <= à la fin)";
    case PORTS_ERROR_MEMORY:
        return "mémoire insuffisante";
    default:
        return "erreur inconnue";
    }
}
