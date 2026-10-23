# PortScan-C

Scanner de ports TCP en ligne de commande, écrit en **C11** avec l'**API
sockets POSIX**, sans aucune bibliothèque externe. Projet de portfolio
(BTS CIEL option A, orientation cybersécurité).

> **ATTENTION : à utiliser uniquement sur des machines qui vous appartiennent
> ou avec autorisation écrite.** Scanner une machine sans autorisation est
> une infraction (en France : article 323-1 du Code pénal). Tous les tests de
> ce projet ont été faits uniquement sur `127.0.0.1`.

## Fonctionnalités

- Scan TCP par **connexion non bloquante** (`connect` + `select`) avec délai
  réglable : chaque port est classé **ouvert**, **fermé** ou **filtré**.
- Liste de ports souple : `80`, `1-1024`, `22,80,443`, `1-100,443,8000-8100`
  (doublons éliminés, ports triés, erreurs claires).
- Nom du **service** (`getservbyport`) et **bannière** du service (première
  ligne envoyée, ou réponse à `HEAD / HTTP/1.0` sur les ports web).
- Rapport **texte** ou **HTML** (avec échappement HTML des données réseau).
- Arrêt propre avec **Ctrl+C** (résumé partiel, code de sortie 130).
- Option bonus **`-j`** : scan multi-thread (pthread), résultats identiques
  au mode simple.
- 0 avertissement de compilation (`-Wall -Wextra -Wpedantic -Werror`),
  0 erreur et 0 fuite sous valgrind, 0 erreur sous helgrind.

## Compilation

Prérequis : Linux, `gcc`, `make` (et `python3`, `valgrind` pour les tests).

```bash
make            # produit l'exécutable ./portscan
make test       # tests unitaires + tests de bout en bout sur 127.0.0.1
make valgrind   # scan de 127.0.0.1 sous valgrind
make clean      # supprime build/ et ./portscan
```

Options de compilation utilisées :
`gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -D_POSIX_C_SOURCE=200809L -pthread`

## Utilisation

```
./portscan <cible> [-p ports] [-t timeout_ms] [-o fichier] [-j threads] [-v] [-h]
```

| Option | Rôle | Défaut |
|--------|------|--------|
| `<cible>` | adresse IPv4 ou nom d'hôte (obligatoire) | - |
| `-p` | ports : `80`, `1-1024`, `22,80,443`, `1-100,443` | `1-1024` |
| `-t` | délai maximum par port, en ms (50 à 10000) | `500` |
| `-o` | rapport : HTML si le nom finit par `.html`, texte sinon | aucun |
| `-j` | nombre de threads (1 à 64) | `1` |
| `-v` | affiche aussi les ports fermés et filtrés | non |
| `-h` | affiche l'aide (code de sortie 0) | - |

Codes de sortie : `0` succès, `1` erreur d'utilisation, `2` nom de cible
impossible à résoudre, `3` erreur système (mémoire, fichier de rapport),
`130` scan interrompu par Ctrl+C.

L'avertissement légal et les messages de progression sont écrits sur la
sortie d'erreur ; le tableau des résultats sur la sortie standard.

## Exemples (sorties réelles)

Les exemples ci-dessous ont été lancés sur `127.0.0.1` avec des serveurs
d'essai : `python3 -m http.server 8080`, un serveur Python sur 9000 qui
envoie `TEST-BANNER-OK`, un autre sur 9001 qui envoie une bannière
contenant du HTML, et un serveur « muet » sur 9100-9139 qui accepte les
connexions sans rien envoyer. Le port 2024 est un service déjà présent sur
la machine de test.

### 1. Scan par défaut (ports 1 à 1024)

```
$ ./portscan 127.0.0.1
ATTENTION : à utiliser uniquement sur des machines qui vous appartiennent ou avec autorisation écrite.
Cible 127.0.0.1 résolue en 127.0.0.1
Scan de 1024 port(s) en cours avec 1 thread(s)...
PortScan-C - cible : 127.0.0.1 (127.0.0.1) - ports : 1-1024 - délai : 500 ms
PORT      ÉTAT     SERVICE     BANNIÈRE
(aucun port ouvert)
----
Résumé : 0 ouverts, 1024 fermés, 0 filtrés - durée : 0.01 s
```

### 2. Liste de ports, mode verbeux, délai court

```
$ ./portscan 127.0.0.1 -p 8080,9000,9001,9998-9999 -v -t 200
ATTENTION : à utiliser uniquement sur des machines qui vous appartiennent ou avec autorisation écrite.
Cible 127.0.0.1 résolue en 127.0.0.1
Scan de 5 port(s) en cours avec 1 thread(s)...
PortScan-C - cible : 127.0.0.1 (127.0.0.1) - ports : 8080,9000,9001,9998-9999 - délai : 200 ms
PORT      ÉTAT     SERVICE     BANNIÈRE
8080/tcp  ouvert   http-alt    HTTP/1.0 200 OK
9000/tcp  ouvert   inconnu     TEST-BANNER-OK
9001/tcp  ouvert   inconnu     <b>gras</b> & 'test'
9998/tcp  fermé    -
9999/tcp  fermé    -
----
Résumé : 3 ouverts, 2 fermés, 0 filtrés - durée : 0.30 s
```

### 3. Rapport texte

```
$ ./portscan 127.0.0.1 -p 8080,9000,9001 -o rapport.txt
...
Rapport écrit dans rapport.txt
$ cat rapport.txt
Date du scan : 2026-10-23 14:23:28
PortScan-C - cible : 127.0.0.1 (127.0.0.1) - ports : 8080,9000,9001 - délai : 500 ms
PORT      ÉTAT     SERVICE     BANNIÈRE
8080/tcp  ouvert   http-alt    HTTP/1.0 200 OK
9000/tcp  ouvert   inconnu     TEST-BANNER-OK
9001/tcp  ouvert   inconnu     <b>gras</b> & 'test'
----
Résumé : 3 ouverts, 0 fermés, 0 filtrés - durée : 0.30 s
```

Avec `-o rapport.html`, la même bannière est échappée dans la page HTML :

```html
<tr><td>9001/tcp</td><td>inconnu</td><td><code>&lt;b&gt;gras&lt;/b&gt; &amp; &#39;test&#39;</code></td></tr>
```

### 4. Erreurs et interruption par Ctrl+C

```
$ ./portscan 127.0.0.1 -p 100-50
ATTENTION : à utiliser uniquement sur des machines qui vous appartiennent ou avec autorisation écrite.
Erreur : liste de ports "100-50" invalide : plage inversée (le début doit être <= à la fin).
(code de sortie : 1)

$ ./portscan nom-invalide.invalid -p 80
ATTENTION : à utiliser uniquement sur des machines qui vous appartiennent ou avec autorisation écrite.
Erreur : impossible de résoudre "nom-invalide.invalid" : Name or service not known
(code de sortie : 2)

$ ./portscan 127.0.0.1 -p 9100-9139 -v      # Ctrl+C (SIGINT) après 1,2 s
PortScan-C - cible : 127.0.0.1 (127.0.0.1) - ports : 9100-9139 - délai : 500 ms
PORT      ÉTAT     SERVICE     BANNIÈRE
9100/tcp  ouvert   inconnu
9101/tcp  ouvert   bacula-dir
9102/tcp  ouvert   bacula-fd
9103/tcp  ouvert   bacula-sd
----
Résumé : 4 ouverts, 0 fermés, 0 filtrés - durée : 1.20 s - scan interrompu (4/40 ports testés)
(code de sortie : 130)
```

### Bonus : effet de `-j` (40 ports muets, 300 ms d'attente de bannière chacun)

```
-j 1  : Résumé : 40 ouverts, 0 fermés, 0 filtrés - durée : 11.99 s
-j 8  : Résumé : 40 ouverts, 0 fermés, 0 filtrés - durée : 1.50 s
-j 32 : Résumé : 40 ouverts, 0 fermés, 0 filtrés - durée : 0.60 s
```

## Tests

- `tests/test_units.c` : 24 tests unitaires avec `assert` (décodage des
  ports, nettoyage des bannières, échappement HTML, conversion de nombres).
- `tests/run_tests.sh` : 55 tests de bout en bout sur `127.0.0.1` uniquement
  (serveurs d'essai Python lancés et toujours arrêtés par `trap EXIT`) :
  détection ouvert/fermé, bannières, rapports texte et HTML, échappement,
  codes de sortie 1/2/3, délai court, Ctrl+C (avec et sans threads),
  20 scans `-j 8` comparés à `-j 1`.

Résultat réel de `make test` : `Tests unitaires : 24 réussis, 0 échoué` puis
`Tests de bout en bout : 55 réussis, 0 échoués`. Le détail complet, les
résumés valgrind et helgrind sont dans [docs/COMPTE_RENDU.md](docs/COMPTE_RENDU.md).

## Structure du dépôt

```
portscan-c/
├── README.md
├── Makefile
├── .gitignore
├── include/
│   ├── ports.h      décodage de la liste de ports
│   ├── scanner.h    types port_state_t / port_result_t, fonctions réseau
│   ├── report.h     structure scan_report_t, affichage et rapports
│   └── utils.h      fonctions utilitaires
├── src/
│   ├── main.c       lecture des arguments, enchaînement des étapes, SIGINT
│   ├── ports.c      décodage de "1-100,443,8000-8100"
│   ├── scanner.c    connexion non bloquante, classement, bannières, threads
│   ├── report.c     affichage terminal, rapport texte et HTML
│   └── utils.c      strtol sûr, nettoyage de bannière, échappement HTML, temps
├── tests/
│   ├── test_units.c tests unitaires (assert)
│   └── run_tests.sh tests de bout en bout sur localhost
└── docs/
    ├── COMPTE_RENDU.md  compte rendu complet du projet
    └── TCP_EXPLIQUE.md  la poignée de main TCP expliquée avec schémas
```

## Limites

- IPv4 uniquement, TCP uniquement (pas d'UDP).
- « Connect scan » : la connexion complète est visible par la cible ; pas de
  scan SYN furtif (il demanderait des sockets bruts et les droits root).
- « Filtré » est déduit d'un simple délai dépassé : un service très lent ou
  un réseau chargé peut être pris pour un port filtré si `-t` est trop court.
- Le nom de service vient de `/etc/services` : il indique le service
  *habituel* du port, pas forcément celui qui tourne vraiment.

## Avertissement légal

Projet à utiliser uniquement sur mes propres machines ou avec autorisation
écrite.
