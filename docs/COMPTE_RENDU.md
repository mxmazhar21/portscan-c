# Compte rendu – PortScan-C

> Projet à utiliser uniquement sur mes propres machines ou avec autorisation écrite.

## 1. Objectif et contexte du projet

PortScan-C est un scanner de ports TCP en ligne de commande, écrit en C11
avec l'API sockets POSIX, sans bibliothèque externe. Il a été réalisé pour
le portfolio d'un étudiant en BTS CIEL option A (orientation cybersécurité).

Objectifs :

- comprendre concrètement le fonctionnement de TCP (poignée de main SYN,
  SYN-ACK, RST) et ce qu'un scanner peut en déduire ;
- programmer proprement en C sous Linux : sockets, appels système, gestion
  d'erreurs, mémoire, signaux et threads ;
- livrer un outil fiable : zéro avertissement de compilation, zéro fuite
  mémoire, tests automatisés.

Tous les tests ont été effectués **uniquement sur 127.0.0.1**.

## 2. Résumé en 5 lignes (pour un recruteur)

1. Scanner de ports TCP en C11 (≈ 1 900 lignes de code source, commentaires compris) : connexion non bloquante, délai réglable, classement ouvert / fermé / filtré.
2. Récupération des bannières de services, noms de services, rapports texte et HTML (avec échappement contre l'injection de code).
3. Robustesse : validation stricte des arguments (strtol), tampons de taille fixe, arrêt propre par Ctrl+C, codes de sortie documentés.
4. Option multi-thread (pthread + mutex) : scan jusqu'à 8 fois plus rapide, résultats identiques au mode simple (vérifié 20 fois).
5. Qualité vérifiée : `-Wall -Wextra -Wpedantic -Werror` sans avertissement, 24 tests unitaires + 55 tests de bout en bout, valgrind/helgrind sans erreur.

## 3. Architecture et rôle de chaque fichier

```
main.c ──> ports.c      (texte "-p" -> tableau de ports trié)
   │
   ├────> scanner.c    (résolution, test de chaque port, bannières, threads)
   │         └──> utils.c (nettoyage de bannière, temps)
   │
   └────> report.c     (terminal, rapport texte, rapport HTML)
             └──> utils.c (échappement HTML, largeur UTF-8)
```

| Fichier | Rôle |
|---------|------|
| `src/main.c` | Lit les options avec `getopt` et `strtol`, affiche l'avertissement légal, installe le gestionnaire de SIGINT, enchaîne : ports → résolution → scan → affichage → rapport, choisit le code de sortie. |
| `src/ports.c` | `parse_port_list` : découpe le texte sur les virgules, décode chaque port ou plage, marque les ports dans un tableau de 65 536 cases, puis construit un tableau trié et sans doublon. |
| `src/scanner.c` | `resolve_target` (getaddrinfo), `connect_with_timeout` (socket non bloquant + select + getsockopt), `scan_port` (test + bannière + close), `scan_ports` (un ou plusieurs threads), `lookup_service_names` (getservbyport). |
| `src/report.c` | Tableau aligné du terminal, rapport texte (même contenu + date), rapport HTML complet. Chaque écriture vérifie le retour de `fprintf`. |
| `src/utils.c` | `parse_long_in_range` (strtol sûr), `sanitize_banner`, `html_escape`, `to_lowercase`, `utf8_extra_bytes`, mesure du temps (CLOCK_MONOTONIC), date (localtime + strftime). |
| `include/*.h` | Déclarations, constantes commentées, types `port_state_t` (PORT_OPEN, PORT_CLOSED, PORT_FILTERED), `port_result_t`, `port_list_t`, `scan_report_t`. |
| `tests/test_units.c` | 24 tests unitaires avec `assert`. |
| `tests/run_tests.sh` | 55 tests de bout en bout sur 127.0.0.1 avec des serveurs d'essai Python. |
| `Makefile` | Compilation séparée dans `build/`, cibles `all`, `test`, `valgrind`, `clean`. |

## 4. Fonctionnement détaillé : le parcours d'un port

Pour chaque port, `scan_port()` (fichier `scanner.c`) fait :

1. **`socket(AF_INET, SOCK_STREAM, 0)`** : crée un socket TCP IPv4. Renvoie
   un descripteur, ou -1 (par exemple `EMFILE` si trop de fichiers sont
   ouverts) ; dans ce cas le port est classé filtré.
2. **`fcntl(F_GETFL)` puis `fcntl(F_SETFL, flags | O_NONBLOCK)`** : passe le
   socket en mode non bloquant.
3. **`connect()`** : le noyau envoie le SYN.
   - retour 0 → connexion immédiate → **ouvert** ;
   - -1 et `errno == EINPROGRESS` → la connexion est en cours, on attend ;
   - -1 et `ECONNREFUSED` immédiat (fréquent sur 127.0.0.1) → **fermé** ;
   - autre erreur → **filtré**.
4. **`select()`** sur l'ensemble « écriture », avec le délai `-t` :
   - 0 → délai dépassé → **filtré** ;
   - -1 avec `EINTR` → un signal est arrivé : si c'est Ctrl+C on abandonne,
     sinon on recommence avec le temps **restant** (échéance calculée avec
     `CLOCK_MONOTONIC`) ;
   - -1 avec une autre erreur → **filtré** ;
   - > 0 → le socket est prêt : la connexion est terminée (réussie ou non).
5. **`getsockopt(SOL_SOCKET, SO_ERROR)`** lit le vrai résultat :
   0 → **ouvert**, `ECONNREFUSED` → **fermé**, autre (`ETIMEDOUT`,
   `EHOSTUNREACH`, `ENETUNREACH`...) → **filtré**.
6. Si le port est ouvert : **bannière**. `select()` en lecture pendant
   300 ms, puis `recv()` de 255 octets maximum. Si rien n'arrive et que le
   port est 80, 8000, 8080 ou 8888, `send()` de `HEAD / HTTP/1.0\r\n\r\n`
   (avec `MSG_NOSIGNAL` pour ne pas recevoir SIGPIPE) puis nouvelle lecture.
   `sanitize_banner()` garde la première ligne et remplace les caractères
   non imprimables par des points.
7. **`close()`** : le socket est toujours fermé, quel que soit l'état (un
   seul point de fermeture dans `scan_port`).

### Pourquoi un `connect` non bloquant avec `select` ?

Un `connect()` bloquant vers un port filtré attend que le noyau abandonne
(plusieurs retransmissions du SYN, souvent plus d'une minute). Avec un
socket non bloquant, `connect()` rend la main tout de suite et c'est
**nous** qui choisissons combien de temps attendre grâce au délai de
`select()`. On garde ainsi un scan rapide et un délai maîtrisé. `select()`
indique « prêt en écriture » quand la poignée de main est terminée, que ce
soit un succès ou un échec : c'est pour cela qu'il faut ensuite lire
`SO_ERROR` pour connaître le résultat réel.

### Le mode multi-thread (`-j`)

`scan_ports()` prépare une structure partagée `scan_job_t` : tableau des
résultats (préalloué, une case par port), nombre de ports, et un **index
`next_index` protégé par un `pthread_mutex_t`**. Chaque thread appelle
`take_next_index()` (verrouillage, lecture + incrément, déverrouillage),
teste le port, puis écrit **uniquement dans sa case** `results[index]`.
Le thread principal attend tous les threads avec `pthread_join()`. Les noms
de service sont cherchés ensuite, par le seul thread principal, car
`getservbyport()` n'est pas thread-safe. Les résultats restent dans l'ordre
des ports, donc l'affichage est identique avec `-j 1` et `-j 8`.

## 5. Notions réseau utilisées

- **TCP** : protocole de transport fiable et orienté connexion ; une
  connexion est identifiée par IP + port de chaque côté.
- **Socket** : point de communication fourni par le système ; en C c'est un
  simple descripteur de fichier (entier) qu'on manipule avec `connect`,
  `send`, `recv`, `close`.
- **SYN** : premier paquet de la poignée de main (« je veux me connecter »).
- **SYN-ACK** : réponse d'un port **ouvert** (« d'accord »).
- **RST** : réponse d'un port **fermé** (« personne n'écoute ici ») ; côté
  programme cela donne l'erreur `ECONNREFUSED`.
- **Timeout** : aucune réponse dans le délai → port **filtré** (paquet jeté
  par un pare-feu, machine éteinte, réseau injoignable).
- **Bannière** : texte qu'un service envoie à la connexion (SSH, FTP, SMTP)
  ou en réponse à une requête (HTTP). Elle révèle souvent le logiciel et sa
  version, information précieuse pour un audit… et pour un attaquant.

Les schémas de la poignée de main sont dans [TCP_EXPLIQUE.md](TCP_EXPLIQUE.md).

## 6. Mesures de robustesse et de sécurité

- **Validation des arguments** : `getopt` pour les options ; `strtol` (jamais
  `atoi`) avec `errno`, `endptr` et bornes : le texte doit commencer par un
  chiffre et être entièrement numérique. Ports de 1 à 65535, délai de 50 à
  10 000 ms, threads de 1 à 64. Plage inversée, liste vide, virgule en trop,
  texte non numérique → message clair + aide + code 1.
- **Tampons** : tailles fixes définies par constantes (`BANNER_SIZE` = 256,
  `SERVICE_SIZE` = 32...) ; `recv` lit au plus `BANNER_SIZE - 1` octets ;
  `snprintf` partout ; aucun `strcpy`, `sprintf`, `gets`, `atoi`.
- **Données réseau** : la bannière est nettoyée (caractères non imprimables
  → `.`), puis **échappée** dans le HTML (`&` `<` `>` `"` `'`) pour empêcher
  une injection de code (XSS) par un service malveillant. Le test avec une
  bannière `<b>gras</b> & 'test'` le prouve. La cible est aussi échappée.
- **Mémoire** : un seul `calloc` pour les résultats et un `malloc` pour la
  liste de ports, chacun libéré sur tous les chemins (y compris les erreurs).
  `freeaddrinfo` après `getaddrinfo`, `endservent` après `getservbyport`.
- **Fichiers** : échec de `fopen`, de `fprintf` ou de `fclose` → message clair
  avec la cause (`strerror`) et code 3 (testé avec `/proc/interdit.txt` et
  avec `/dev/full`, qui simule un disque plein).
- **SIGINT** : `sigaction` sans `SA_RESTART` → `select` est interrompu
  (`EINTR`) ; le gestionnaire ne fait que lever le drapeau
  `volatile sig_atomic_t g_stop_requested` (seule variable globale). Le scan
  s'arrête, le résultat du port en cours est ignoré, le résumé partiel est
  affiché avec « scan interrompu », la mémoire est libérée, code 130.
- **SIGPIPE** : `send(..., MSG_NOSIGNAL)` évite que le programme soit tué si
  un serveur ferme la connexion avant la requête HTTP.
- **Threads** : un seul verrou pour l'index, aucune autre écriture partagée,
  tous les threads attendus par `pthread_join` ; si `pthread_create` échoue,
  les threads déjà créés (ou le thread principal) terminent le travail.

## 7. Tests réalisés (sorties réelles)

Environnement : Linux 6.18 (conteneur Ubuntu 24.04), gcc 13.3.0,
valgrind 3.22.0, Python 3.13.

### 7.1 Compilation (`make clean` puis `make`)

```
rm -rf build portscan
mkdir -p build
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -D_POSIX_C_SOURCE=200809L -pthread -Iinclude -c src/main.c -o build/main.o
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -D_POSIX_C_SOURCE=200809L -pthread -Iinclude -c src/ports.c -o build/ports.o
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -D_POSIX_C_SOURCE=200809L -pthread -Iinclude -c src/scanner.c -o build/scanner.o
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -D_POSIX_C_SOURCE=200809L -pthread -Iinclude -c src/report.c -o build/report.o
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -D_POSIX_C_SOURCE=200809L -pthread -Iinclude -c src/utils.c -o build/utils.o
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -D_POSIX_C_SOURCE=200809L -pthread -Iinclude build/main.o build/ports.o build/scanner.o build/report.o build/utils.o -o portscan -pthread
```

Aucun avertissement (et `-Werror` transformerait tout avertissement en
erreur).

### 7.2 `make test` (sortie complète)

```
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -D_POSIX_C_SOURCE=200809L -pthread -Iinclude -c tests/test_units.c -o build/test_units.o
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -D_POSIX_C_SOURCE=200809L -pthread -Iinclude build/test_units.o build/ports.o build/utils.o -o build/test_units -pthread
./build/test_units
PASS : port seul "80"
PASS : plage "20-23"
PASS : liste "22,80,443"
PASS : mélange "1-3,443,8000-8001"
PASS : doublons éliminés
PASS : ports triés
PASS : plage complète "1-65535"
PASS : refus de "0"
PASS : refus de "65536"
PASS : refus de "-5"
PASS : refus de "abc"
PASS : refus de la liste vide
PASS : refus de "80,"
PASS : refus de la plage inversée "100-50"
PASS : refus d'un nombre géant
PASS : conversion de nombre avec bornes
PASS : bannière : caractères non imprimables remplacés
PASS : bannière : seule la première ligne est gardée
PASS : bannière : chaîne vide
PASS : bannière : chaîne trop longue coupée sans débordement
PASS : HTML : les 5 caractères spéciaux
PASS : HTML : chaîne vide
PASS : HTML : "<script>alert(1)</script>"
PASS : HTML : tampon trop petit, entité jamais coupée
Tests unitaires : 24 réussis, 0 échoué
bash tests/run_tests.sh
== Démarrage des serveurs d'essai sur 127.0.0.1 ==
Ports testés : 8080,9000,9001,9999 (port fermé choisi : 9999)
== Détection des ports et bannières ==
PASS : scan avec -v : code de sortie 0
PASS : 8080 détecté ouvert
PASS : 9000 détecté ouvert
PASS : 9999 détecté fermé (avec -v)
PASS : bannière TEST-BANNER-OK sur 9000
PASS : bannière HTTP sur 8080 (requête HEAD)
PASS : service http-alt sur 8080
PASS : résumé : 3 ouverts, 1 fermés
PASS : avertissement légal sur la sortie d'erreur
PASS : sans -v : le port fermé n'est pas listé
== Rapports ==
PASS : rapport texte : code de sortie 0
PASS : rapport texte créé
PASS : rapport texte : date du scan
PASS : rapport texte : bannière TEST-BANNER-OK
PASS : rapport texte : résumé
PASS : rapport HTML : code de sortie 0
PASS : rapport HTML créé avec doctype
PASS : rapport HTML : meta charset UTF-8
PASS : rapport HTML : port 9000 et sa bannière
PASS : rapport HTML : bannière <b> échappée
PASS : rapport HTML : aucune balise <b> venue du réseau
PASS : rapport HTML : port fermé absent du tableau
PASS : rapport HTML : page complète
== Erreurs d'arguments et codes de sortie ==
PASS : aide -h (code 0, attendu 0)
PASS : port 0 (code 1, attendu 1)
PASS : port 70000 (code 1, attendu 1)
PASS : plage inversée 100-50 (code 1, attendu 1)
PASS : liste avec virgule en trop (code 1, attendu 1)
PASS : port non numérique (code 1, attendu 1)
PASS : timeout négatif (code 1, attendu 1)
PASS : timeout trop petit (code 1, attendu 1)
PASS : timeout non numérique (code 1, attendu 1)
PASS : cible absente (code 1, attendu 1)
PASS : deux cibles (code 1, attendu 1)
PASS : option inconnue -z (code 1, attendu 1)
PASS : valeur manquante pour -p (code 1, attendu 1)
PASS : cible inexistante nom-invalide.invalid (code 2, attendu 2)
PASS : rapport impossible à écrire /proc/interdit.txt (code 3, attendu 3)
PASS : rapport sur un disque plein (/dev/full) (code 3, attendu 3)
== Délai très court ==
PASS : timeout 50 ms : code de sortie 0
PASS : timeout 50 ms : 9000 toujours ouvert
PASS : timeout 50 ms : 9999 toujours fermé
PASS : timeout 50 ms : 4 ports classés
== Interruption par Ctrl+C (SIGINT) ==
PASS : SIGINT : code de sortie 130 (obtenu 130)
PASS : SIGINT : mention scan interrompu
PASS : SIGINT : résumé partiel affiché
PASS : SIGINT avec -j 4 : code de sortie 130 (obtenu 130)
PASS : SIGINT avec -j 4 : mention scan interrompu
PASS : SIGINT avec -j 4 : résumé partiel affiché
== Option -j (threads) ==
PASS : -j 0 refusé (code 1, attendu 1)
PASS : -j 65 refusé (code 1, attendu 1)
PASS : -j abc refusé (code 1, attendu 1)
PASS : -j 64 accepté (code 0, attendu 0)
PASS : -j 1 : la référence contient 9000 ouvert
PASS : -j 8 : 20/20 scans identiques à -j 1
== Bilan ==
Tests de bout en bout : 55 réussis, 0 échoués
```

### 7.3 Valgrind, helgrind et DRD

Commande utilisée : `valgrind --leak-check=full --show-leak-kinds=all
--error-exitcode=1`. Des serveurs d'essai tournaient sur 8080, 9000, 9001 et
9100-9139 (127.0.0.1). Le chemin du dossier temporaire a été abrégé en
`$TMP`.

```
### Scénario 1 : scan simple avec ports ouverts (-j 1)
$ valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 ./portscan 127.0.0.1 -p 1-1024,8080,9000,9001
code de sortie : 0
PortScan-C - cible : 127.0.0.1 (127.0.0.1) - ports : 1-1024,8080,9000,9001 - délai : 500 ms
PORT      ÉTAT     SERVICE     BANNIÈRE
8080/tcp  ouvert   http-alt    HTTP/1.0 200 OK
9000/tcp  ouvert   inconnu     TEST-BANNER-OK
9001/tcp  ouvert   inconnu     <b>gras</b> & 'test'
----
Résumé : 3 ouverts, 1024 fermés, 0 filtrés - durée : 0.33 s
    in use at exit: 0 bytes in 0 blocks
  total heap usage: 34 allocs, 34 frees, 409,232 bytes allocated
All heap blocks were freed -- no leaks are possible
ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)

### Scénario 2 : scan avec rapport HTML
$ valgrind ... ./portscan 127.0.0.1 -p 1-1024,8080,9000,9001 -o $TMP/vg2.html
code de sortie : 0
    in use at exit: 0 bytes in 0 blocks
  total heap usage: 36 allocs, 36 frees, 413,800 bytes allocated
All heap blocks were freed -- no leaks are possible
ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)

### Scénario 3 : scan interrompu par SIGINT
$ valgrind ... ./portscan 127.0.0.1 -p 9100-9139 -v &  puis  kill -INT après 2 s
code de sortie : 130
----
Résumé : 4 ouverts, 0 fermés, 0 filtrés - durée : 1.44 s - scan interrompu (4/40 ports testés)
    in use at exit: 0 bytes in 0 blocks
  total heap usage: 36 allocs, 36 frees, 113,740 bytes allocated
All heap blocks were freed -- no leaks are possible
ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)

### Scénario 4 : scan multi-thread -j 8 sous memcheck
code de sortie : 0
    in use at exit: 0 bytes in 0 blocks
  total heap usage: 44 allocs, 44 frees, 415,976 bytes allocated
All heap blocks were freed -- no leaks are possible
ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)

### Scénario 5 : SIGINT pendant un scan -j 4 sous memcheck
code de sortie : 130
Résumé : 12 ouverts, 0 fermés, 0 filtrés - durée : 1.34 s - scan interrompu (12/40 ports testés)
    in use at exit: 0 bytes in 0 blocks
  total heap usage: 56 allocs, 56 frees, 151,372 bytes allocated
All heap blocks were freed -- no leaks are possible
ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)

### Helgrind -j 8
$ valgrind --tool=helgrind --error-exitcode=1 ./portscan 127.0.0.1 -p 1-1024,8080,9000,9001 -j 8
code de sortie : 0
ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 5920 from 11)

### DRD -j 8
code de sortie : 0
ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 15 from 5)
```

Les trois scénarios demandés (scan simple, rapport HTML, SIGINT) et les
deux scénarios multi-thread donnent **0 erreur et 0 octet perdu** (« All
heap blocks were freed -- no leaks are possible »). Helgrind et DRD
(détecteurs de problèmes de threads) ne trouvent **aucune erreur** avec
`-j 8` ; les lignes « suppressed » correspondent aux suppressions fournies
par défaut avec valgrind pour la glibc, pas à des erreurs de PortScan-C.

### 7.4 Exemples de scans

Port **filtré** (démonstration manuelle : règle pare-feu temporaire sur
l'interface locale, retirée juste après) :

```
$ iptables -I INPUT -i lo -p tcp --dport 9997 -j DROP
$ ./portscan 127.0.0.1 -p 9996-9998 -v -t 300
ATTENTION : à utiliser uniquement sur des machines qui vous appartiennent ou avec autorisation écrite.
Cible 127.0.0.1 résolue en 127.0.0.1
Scan de 3 port(s) en cours avec 1 thread(s)...
PortScan-C - cible : 127.0.0.1 (127.0.0.1) - ports : 9996-9998 - délai : 300 ms
PORT      ÉTAT     SERVICE     BANNIÈRE
9996/tcp  fermé    -
9997/tcp  filtré   -
9998/tcp  fermé    -
----
Résumé : 0 ouverts, 2 fermés, 1 filtrés - durée : 0.30 s
$ iptables -D INPUT -i lo -p tcp --dport 9997 -j DROP
```

La durée de 0,30 s correspond exactement au délai `-t 300` : le scanner a
attendu une réponse qui n'est jamais venue. Le même scan sous valgrind :
`ERROR SUMMARY: 0 errors`, `All heap blocks were freed`.

Autres exemples réels (sortie complète du script d'exemples ; chaque bloc
commence par `=== EXn`, puis la commande lancée et sa sortie) :

```
=== EX1
$ ./portscan 127.0.0.1
ATTENTION : à utiliser uniquement sur des machines qui vous appartiennent ou avec autorisation écrite.
Cible 127.0.0.1 résolue en 127.0.0.1
Scan de 1024 port(s) en cours avec 1 thread(s)...
PortScan-C - cible : 127.0.0.1 (127.0.0.1) - ports : 1-1024 - délai : 500 ms
PORT      ÉTAT     SERVICE     BANNIÈRE
(aucun port ouvert)
----
Résumé : 0 ouverts, 1024 fermés, 0 filtrés - durée : 0.01 s
(code de sortie : 0)

=== EX2
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
(code de sortie : 0)

=== EX3
$ ./portscan localhost -p 1-10000 -j 8 -o rapport.html
ATTENTION : à utiliser uniquement sur des machines qui vous appartiennent ou avec autorisation écrite.
Cible localhost résolue en 127.0.0.1
Scan de 10000 port(s) en cours avec 8 thread(s)...
Rapport écrit dans rapport.html
PortScan-C - cible : localhost (127.0.0.1) - ports : 1-10000 - délai : 500 ms
PORT      ÉTAT     SERVICE     BANNIÈRE
2024/tcp  ouvert   inconnu
8080/tcp  ouvert   http-alt    HTTP/1.0 200 OK
9000/tcp  ouvert   inconnu     TEST-BANNER-OK
9001/tcp  ouvert   inconnu     <b>gras</b> & 'test'
9100/tcp  ouvert   inconnu
9101/tcp  ouvert   bacula-dir
9102/tcp  ouvert   bacula-fd
9103/tcp  ouvert   bacula-sd
9104/tcp  ouvert   inconnu
9105/tcp  ouvert   inconnu
9106/tcp  ouvert   inconnu
9107/tcp  ouvert   inconnu
9108/tcp  ouvert   inconnu
9109/tcp  ouvert   inconnu
9110/tcp  ouvert   inconnu
9111/tcp  ouvert   inconnu
9112/tcp  ouvert   inconnu
9113/tcp  ouvert   inconnu
9114/tcp  ouvert   inconnu
9115/tcp  ouvert   inconnu
9116/tcp  ouvert   inconnu
9117/tcp  ouvert   inconnu
9118/tcp  ouvert   inconnu
9119/tcp  ouvert   inconnu
9120/tcp  ouvert   inconnu
9121/tcp  ouvert   inconnu
9122/tcp  ouvert   inconnu
9123/tcp  ouvert   inconnu
9124/tcp  ouvert   inconnu
9125/tcp  ouvert   inconnu
9126/tcp  ouvert   inconnu
9127/tcp  ouvert   inconnu
9128/tcp  ouvert   inconnu
9129/tcp  ouvert   inconnu
9130/tcp  ouvert   inconnu
9131/tcp  ouvert   inconnu
9132/tcp  ouvert   inconnu
9133/tcp  ouvert   inconnu
9134/tcp  ouvert   inconnu
9135/tcp  ouvert   inconnu
9136/tcp  ouvert   inconnu
9137/tcp  ouvert   inconnu
9138/tcp  ouvert   inconnu
9139/tcp  ouvert   inconnu
----
Résumé : 44 ouverts, 9956 fermés, 0 filtrés - durée : 1.83 s
(code de sortie : 0)

=== EX3b
$ ./portscan 127.0.0.1 -p 8080,9000,9001 -o rapport.txt
ATTENTION : à utiliser uniquement sur des machines qui vous appartiennent ou avec autorisation écrite.
Cible 127.0.0.1 résolue en 127.0.0.1
Scan de 3 port(s) en cours avec 1 thread(s)...
Rapport écrit dans rapport.txt
PortScan-C - cible : 127.0.0.1 (127.0.0.1) - ports : 8080,9000,9001 - délai : 500 ms
PORT      ÉTAT     SERVICE     BANNIÈRE
8080/tcp  ouvert   http-alt    HTTP/1.0 200 OK
9000/tcp  ouvert   inconnu     TEST-BANNER-OK
9001/tcp  ouvert   inconnu     <b>gras</b> & 'test'
----
Résumé : 3 ouverts, 0 fermés, 0 filtrés - durée : 0.30 s
(code de sortie : 0)

Date du scan : 2026-10-23 14:23:28
PortScan-C - cible : 127.0.0.1 (127.0.0.1) - ports : 8080,9000,9001 - délai : 500 ms
PORT      ÉTAT     SERVICE     BANNIÈRE
8080/tcp  ouvert   http-alt    HTTP/1.0 200 OK
9000/tcp  ouvert   inconnu     TEST-BANNER-OK
9001/tcp  ouvert   inconnu     <b>gras</b> & 'test'
----
Résumé : 3 ouverts, 0 fermés, 0 filtrés - durée : 0.30 s

<table>
<tr><th>Port</th><th>Service</th><th>Bannière</th></tr>
<tr><td>2024/tcp</td><td>inconnu</td><td><code></code></td></tr>
<tr><td>8080/tcp</td><td>http-alt</td><td><code>HTTP/1.0 200 OK</code></td></tr>
<tr><td>9000/tcp</td><td>inconnu</td><td><code>TEST-BANNER-OK</code></td></tr>
<tr><td>9001/tcp</td><td>inconnu</td><td><code>&lt;b&gt;gras&lt;/b&gt; &amp; &#39;test&#39;</code></td></tr>
<tr><td>9100/tcp</td><td>inconnu</td><td><code></code></td></tr>
=== EX4
$ ./portscan 127.0.0.1 -p 100-50
ATTENTION : à utiliser uniquement sur des machines qui vous appartiennent ou avec autorisation écrite.
Erreur : liste de ports "100-50" invalide : plage inversée (le début doit être <= à la fin).
(code de sortie : 1)

$ ./portscan nom-invalide.invalid -p 80
ATTENTION : à utiliser uniquement sur des machines qui vous appartiennent ou avec autorisation écrite.
Erreur : impossible de résoudre "nom-invalide.invalid" : Name or service not known
(code de sortie : 2)

$ ./portscan 127.0.0.1 -p 80 -o /proc/interdit.txt
ATTENTION : à utiliser uniquement sur des machines qui vous appartiennent ou avec autorisation écrite.
Cible 127.0.0.1 résolue en 127.0.0.1
Scan de 1 port(s) en cours avec 1 thread(s)...
Erreur : impossible d'écrire le rapport "/proc/interdit.txt" : No such file or directory
PortScan-C - cible : 127.0.0.1 (127.0.0.1) - ports : 80 - délai : 500 ms
PORT      ÉTAT     SERVICE     BANNIÈRE
(aucun port ouvert)
----
Résumé : 0 ouverts, 1 fermés, 0 filtrés - durée : 0.00 s
(code de sortie : 3)

=== EX5 sigint
code=130
ATTENTION : à utiliser uniquement sur des machines qui vous appartiennent ou avec autorisation écrite.
Cible 127.0.0.1 résolue en 127.0.0.1
Scan de 40 port(s) en cours avec 1 thread(s)...
PortScan-C - cible : 127.0.0.1 (127.0.0.1) - ports : 9100-9139 - délai : 500 ms
PORT      ÉTAT     SERVICE     BANNIÈRE
9100/tcp  ouvert   inconnu
9101/tcp  ouvert   bacula-dir
9102/tcp  ouvert   bacula-fd
9103/tcp  ouvert   bacula-sd
----
Résumé : 4 ouverts, 0 fermés, 0 filtrés - durée : 1.20 s - scan interrompu (4/40 ports testés)
=== EX6 j1 vs j8 timing
Résumé : 40 ouverts, 0 fermés, 0 filtrés - durée : 11.99 s
Résumé : 40 ouverts, 0 fermés, 0 filtrés - durée : 1.50 s
Résumé : 40 ouverts, 0 fermés, 0 filtrés - durée : 0.60 s
=== EX7 help
$ ./portscan -h
ATTENTION : à utiliser uniquement sur des machines qui vous appartiennent ou avec autorisation écrite.
Usage : ./portscan <cible> [-p ports] [-t timeout_ms] [-o fichier] [-j threads] [-v] [-h]
  <cible>  adresse IPv4 ou nom d'hôte à scanner
  -p       ports : 80, 1-1024, 22,80,443 ou 1-100,443 (défaut : 1-1024)
  -t       délai par port en ms, de 50 à 10000 (défaut : 500)
  -o       rapport : HTML si le nom finit par .html, texte sinon
  -j       nombre de threads, de 1 à 64 (défaut : 1)
  -v       affiche aussi les ports fermés et filtrés
  -h       affiche cette aide
(code de sortie : 0)
```

Remarque : le port 2024 est un service déjà présent sur la machine de
test ; les ports 9100-9139 sont le serveur « muet » de test (il accepte les
connexions sans rien envoyer : chaque port coûte les 300 ms d'attente de
bannière, ce qui montre bien le gain de `-j`).

## 8. Choix et hypothèses

1. **Colonne PORT de 10 caractères** au lieu de 8 dans l'exemple de
   l'énoncé : avec 8, `8080/tcp` (8 caractères) serait collé à l'état et
   `65535/tcp` décalerait les colonnes. Un espace de séparation est toujours
   garanti entre colonnes.
2. **Alignement avec les accents** : `printf("%-*s")` compte des octets, et
   « é » occupe 2 octets en UTF-8. `utf8_extra_bytes()` corrige la largeur
   pour que `fermé`, `filtré`, `ÉTAT` restent alignés.
3. **Cible à n'importe quelle position** : avec `-D_POSIX_C_SOURCE`, glibc
   fournit le `getopt` POSIX qui s'arrête au premier argument non-option ;
   la boucle de `parse_options` prend cet argument comme cible et continue.
   Une deuxième cible est refusée (code 1).
4. **Nombres stricts** : le texte doit commencer par un chiffre ; `" 80"`,
   `"+80"`, `"-5"`, `"12ms"` sont refusés.
5. **`ECONNREFUSED` immédiat** au `connect()` → fermé (cas réel sur
   127.0.0.1, non décrit dans l'énoncé) ; autre erreur immédiate → filtré.
   Un échec de `socket()`/`fcntl()` classe aussi le port en filtré.
6. **`EINTR` sur `select`** : on recommence avec le temps restant (pas le
   délai complet), sauf si Ctrl+C a été demandé.
7. **Bannière HTTP** : la requête `HEAD` n'est envoyée que si rien n'est
   arrivé en 300 ms (pas si le serveur a déjà fermé) ; on attend ensuite
   encore au plus 300 ms la réponse.
8. **Nom de service** : cherché seulement pour les ports ouverts (en `-v`, les
   ports fermés/filtrés affichent `-`), après le scan, dans le thread
   principal (`getservbyport` n'est pas thread-safe).
9. **Sorties** : avertissement légal, progression et erreurs sur la sortie
   d'erreur ; tableau sur la sortie standard (on peut rediriger le tableau
   seul). L'avertissement est affiché à chaque lancement, même avec `-h`.
10. **Rapport texte** : il suit `-v` comme le terminal. Le **rapport HTML**
    ne contient que les ports ouverts (comme demandé). La cible est aussi
    échappée en HTML ; l'extension `.html` est sensible à la casse.
11. **Interruption** : le rapport est quand même écrit (résumé partiel). Si
    l'écriture du rapport échoue, le code 3 est prioritaire sur 130. Le port
    en cours de test au moment du Ctrl+C n'est pas compté (sinon il serait
    faussement « filtré »).
12. **`-j 1`** n'utilise pas de thread : le thread principal exécute le même
    code de travail. Si `pthread_create` échoue, un avertissement est
    affiché et les threads déjà lancés (ou le thread principal) terminent.
13. **Messages système** (`strerror`, `gai_strerror`) en anglais : pas de
    `setlocale`, pour que `isprint`/`tolower` gardent le comportement de la
    locale « C » (tout octet non ASCII d'une bannière devient un point, ce
    qui garantit aussi un HTML en UTF-8 valide).
14. **Tests** : un serveur Python « muet » (ports 9100-9139) a été ajouté
    pour tester Ctrl+C de façon fiable (sans lui, un scan de localhost se
    termine trop vite pour être interrompu). Si un port d'essai est déjà
    occupé, le script s'arrête avec un message au lieu de donner de faux
    résultats. Les 20 comparaisons `-j 8` / `-j 1` sont intégrées à
    `make test`.
15. **État filtré** : démontré manuellement avec une règle iptables
    temporaire sur `lo` ; ce test n'est pas dans `run_tests.sh` car il
    demande les droits root et modifie le pare-feu.
16. **Valgrind** était installé : les sanitizers (`-fsanitize`) n'ont pas été
    nécessaires.
17. Les fichiers objets sont compilés dans `build/` ; `make clean` supprime
    `build/` et `./portscan`.

## 9. Limites connues et pistes d'amélioration

**Limites :**

- **IPv4 seulement** (`AF_INET`) ; pas d'IPv6.
- **TCP seulement** : pas de scan UDP (plus difficile : un port UDP ouvert
  ne répond souvent rien).
- **Pas de scan SYN furtif** : PortScan-C fait un « connect scan » ; la
  connexion complète est journalisée par la cible. Un scan SYN (comme
  `nmap -sS`) demande des sockets bruts et les droits root.
- **Filtrage détecté par simple délai dépassé** : un réseau lent ou un
  service surchargé peut être pris pour « filtré » si `-t` est trop court.
- `select()` est limité aux descripteurs < `FD_SETSIZE` (1024) : sans
  risque ici (au plus 64 sockets ouverts en même temps), mais `poll()`
  serait plus général.
- Le drapeau `volatile sig_atomic_t` est garanti par la norme C pour le
  thread qui reçoit le signal ; sa lecture par les autres threads fonctionne
  sous Linux mais une variable `_Atomic` serait plus rigoureuse.
- Le nom de service vient de `/etc/services` : c'est le service *habituel*
  du port, pas forcément celui qui tourne réellement (la bannière est plus
  fiable).

**Pistes d'amélioration :** IPv6 (`AF_UNSPEC`), `poll()` ou `epoll` pour
lancer des centaines de connexions sans threads, scan UDP, export JSON/CSV,
détection de version plus fine à partir des bannières, plusieurs cibles
(plage d'adresses), limitation du débit pour rester discret sur un réseau.

## 10. Compétences démontrées

- **Langage C** : C11 strict, découpage en modules et en fonctions courtes,
  structures, énumérations, gestion rigoureuse de la mémoire et des chaînes.
- **Réseau** : sockets POSIX, TCP, poignée de main, connexion non bloquante,
  `select`, `getsockopt(SO_ERROR)`, résolution DNS (`getaddrinfo`),
  services, bannières.
- **Système Linux** : appels système et `errno`, descripteurs de fichiers,
  signaux (`sigaction`, `EINTR`, SIGPIPE), threads POSIX (mutex, join),
  horloge monotone, Makefile, pare-feu (iptables pour la démonstration).
- **Sécurité** : validation des entrées, prévention des débordements,
  échappement HTML (XSS), cadre légal du scan de ports.
- **Débogage et qualité** : valgrind (memcheck), helgrind, DRD, compilation
  `-Werror`, tests unitaires (`assert`) et de bout en bout (bash + Python),
  Git avec commits par étape.

## 11. Questions d'entretien probables (et réponses)

1. **Pourquoi un `connect()` non bloquant ?**
   Un `connect()` bloquant vers un port filtré peut attendre plus d'une
   minute. En non bloquant, `connect()` renvoie tout de suite `EINPROGRESS`
   et `select()` attend au plus le délai choisi avec `-t`.

2. **Quelle différence entre un port fermé et un port filtré ?**
   Fermé : la machine répond par un RST (erreur `ECONNREFUSED`), c'est
   immédiat. Filtré : aucune réponse dans le délai (pare-feu qui jette le
   paquet) ou erreur réseau comme `EHOSTUNREACH`.

3. **Pourquoi `getsockopt(SO_ERROR)` après `select()` ?**
   `select()` signale « prêt en écriture » quand la connexion est terminée,
   qu'elle ait réussi ou échoué. Seul `SO_ERROR` donne le résultat : 0 =
   ouvert, `ECONNREFUSED` = fermé, autre = filtré.

4. **Pourquoi surveiller l'écriture et pas la lecture pendant `connect` ?**
   Un socket devient inscriptible quand la poignée de main est finie. La
   lecture n'est utilisée qu'ensuite, pour attendre la bannière.

5. **Pourquoi `strtol` et pas `atoi` ?**
   `atoi` ne signale aucune erreur (`atoi("abc")` vaut 0). Avec `strtol` je
   vérifie `errno` (dépassement), `endptr` (caractères en trop) et les
   bornes, dans `parse_long_in_range()`.

6. **Comment triez-vous les ports et retirez-vous les doublons ?**
   `parse_port_list()` marque chaque port demandé dans un tableau de 65 536
   octets (`seen[port] = 1`), puis parcourt ce tableau de 1 à 65535 : les
   ports sortent triés et chacun une seule fois. Pas besoin de `qsort`.

7. **Comment évitez-vous les débordements de tampon ?**
   Tampons de taille fixe définie par constante, `recv` limité à
   `BANNER_SIZE - 1`, `snprintf` partout, `sanitize_banner` et
   `html_escape` qui ne dépassent jamais la taille du tampon de sortie (et
   ne coupent jamais une entité HTML). Aucun `strcpy`/`sprintf`/`gets`.

8. **Pourquoi échapper la bannière dans le rapport HTML ?**
   La bannière vient du réseau : un service malveillant pourrait envoyer
   `<script>...</script>` et faire exécuter du JavaScript à celui qui ouvre
   le rapport (XSS). `html_escape` remplace `& < > " '` par leurs entités.

9. **Comment fonctionne l'arrêt par Ctrl+C ?**
   `sigaction` installe `handle_sigint`, qui met seulement
   `g_stop_requested` à 1. Sans `SA_RESTART`, `select` est interrompu avec
   `EINTR`. La boucle de scan voit le drapeau et s'arrête ; on affiche le
   résumé partiel « scan interrompu », on libère la mémoire et on renvoie 130.

10. **Pourquoi `volatile sig_atomic_t` ?**
    `sig_atomic_t` peut être lu/écrit en une seule opération même si un
    signal arrive ; `volatile` empêche le compilateur de garder la valeur
    en registre, donc la boucle relit vraiment la variable.

11. **Comment les threads se partagent-ils le travail sans conflit ?**
    Un seul élément partagé modifiable : l'index du prochain port, protégé
    par un mutex dans `take_next_index()`. Chaque thread écrit seulement
    dans `results[index]`, sa propre case. Le thread principal fait
    `pthread_join` sur tous les threads avant de lire les résultats.
    Helgrind et DRD ne signalent aucune erreur.

12. **Pourquoi chercher les noms de service après le scan ?**
    `getservbyport()` renvoie un pointeur vers une zone statique : il n'est
    pas thread-safe. Il est donc appelé après les `pthread_join`, par le seul
    thread principal, suivi de `endservent()`.

13. **À quoi sert `MSG_NOSIGNAL` ?**
    Si le serveur a fermé la connexion, `send()` déclencherait le signal
    SIGPIPE qui tue le programme. Avec `MSG_NOSIGNAL`, `send` renvoie
    simplement -1 (`EPIPE`).

14. **Comment avez-vous vérifié l'absence de fuites et d'erreurs mémoire ?**
    Valgrind `--leak-check=full --error-exitcode=1` sur un scan simple, un
    scan avec rapport HTML, un scan interrompu par SIGINT et des scans
    `-j 4` / `-j 8` : 0 erreur, « All heap blocks were freed ». Helgrind et
    DRD pour les threads.

15. **Votre scan est-il discret ? Quelle différence avec `nmap -sS` ?**
    Non : c'est un « connect scan », la poignée de main va jusqu'au bout et
    la cible peut journaliser la connexion. `nmap -sS` envoie un SYN avec un
    socket brut, lit la réponse, puis envoie un RST sans finir la connexion :
    plus discret mais il faut les droits root.

16. **(Bonus) Pourquoi `CLOCK_MONOTONIC` pour mesurer la durée ?**
    Cette horloge ne recule jamais, même si l'heure du système est modifiée
    (NTP, changement manuel) : la durée et les délais restent justes.

---

**Projet à utiliser uniquement sur mes propres machines ou avec autorisation écrite.**
