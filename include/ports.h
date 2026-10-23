/*
 * Fichier : ports.h
 * Ce fichier sert à déclarer le décodage de la liste de ports donnée avec
 * l'option -p (exemples : "80", "1-1024", "22,80,443", "1-100,443").
 */
#ifndef PORTS_H
#define PORTS_H

#include <stddef.h>

/* Plus petit numéro de port TCP valide. */
#define PORT_MIN 1
/* Plus grand numéro de port TCP valide (16 bits). */
#define PORT_MAX 65535
/* Ports scannés quand l'option -p n'est pas donnée. */
#define DEFAULT_PORT_SPEC "1-1024"

/* Liste de ports triée et sans doublon. */
typedef struct {
    int *ports;   /* tableau alloué avec malloc */
    size_t count; /* nombre de cases du tableau */
} port_list_t;

/* Résultat du décodage de la liste de ports. */
typedef enum {
    PORTS_OK = 0,
    PORTS_ERROR_SYNTAX,  /* texte vide, lettre, virgule en trop... */
    PORTS_ERROR_RANGE,   /* port en dehors de 1 à 65535 */
    PORTS_ERROR_ORDER,   /* plage inversée comme "100-50" */
    PORTS_ERROR_MEMORY   /* allocation mémoire impossible */
} port_error_t;

port_error_t parse_port_list(const char *text, port_list_t *list);
void free_port_list(port_list_t *list);
const char *port_error_message(port_error_t error);

#endif
