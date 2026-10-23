/*
 * Fichier : report.h
 * Ce fichier sert à déclarer l'affichage des résultats : tableau dans le
 * terminal, rapport texte et rapport HTML (option -o).
 */
#ifndef REPORT_H
#define REPORT_H

#include <stddef.h>
#include <stdio.h>

#include "scanner.h"
#include "utils.h"

/* Toutes les informations nécessaires pour afficher un scan. */
typedef struct {
    const char *target;            /* cible telle que tapée par l'utilisateur */
    const char *ip_text;           /* adresse IP résolue en texte */
    const char *port_spec;         /* texte de l'option -p */
    int timeout_ms;                /* délai par port */
    int verbose;                   /* 1 : afficher aussi fermés et filtrés */
    int interrupted;               /* 1 : scan arrêté par Ctrl+C */
    double duration;               /* durée du scan en secondes */
    char date[DATE_SIZE];          /* date et heure du scan */
    const port_result_t *results;  /* résultats, triés par port */
    size_t count;                  /* nombre de ports demandés */
    size_t tested;                 /* nombre de ports testés jusqu'au bout */
} scan_report_t;

int print_report(FILE *out, const scan_report_t *report);
int write_report_file(const char *path, const scan_report_t *report);

#endif
