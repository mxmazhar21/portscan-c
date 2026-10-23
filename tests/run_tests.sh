#!/usr/bin/env bash
# Fichier : run_tests.sh
# Ce fichier sert à tester PortScan-C de bout en bout, UNIQUEMENT sur
# 127.0.0.1 : il démarre des petits serveurs d'essai en arrière-plan, lance
# le scanner, vérifie ses sorties avec grep et ses codes de sortie, puis
# arrête toujours les serveurs (trap EXIT). Il affiche PASS ou FAIL pour
# chaque test et renvoie un code non nul au moindre échec.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BIN="$PROJECT_DIR/portscan"
WORK_DIR="$(mktemp -d)"
TARGET="127.0.0.1"
HTTP_PORT=8080      # serveur HTTP de Python
BANNER_PORT=9000    # serveur qui envoie "TEST-BANNER-OK"
HTML_PORT=9001      # serveur qui envoie une bannière avec du HTML
SILENT_FIRST=9100   # premier port du serveur muet (test de Ctrl+C)
SILENT_LAST=9139    # dernier port du serveur muet
THREAD_RUNS=20      # nombre de comparaisons -j 8 / -j 1
PASS_COUNT=0
FAIL_COUNT=0
SERVER_PIDS=()

# Petit serveur Python :
#  - "banner PORT TEXTE" : envoie TEXTE à chaque connexion puis ferme ;
#  - "silent PORT..." : écoute sur les ports sans jamais rien envoyer.
SERVER_CODE='
import socket, sys, time
def listen(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(64)
    return s
if sys.argv[1] == "banner":
    server = listen(int(sys.argv[2]))
    message = sys.argv[3].encode() + b"\r\n"
    while True:
        conn, _ = server.accept()
        try:
            conn.sendall(message)
        except OSError:
            pass
        conn.close()
else:
    sockets = [listen(int(port)) for port in sys.argv[2:]]
    while True:
        time.sleep(60)
'

# Arrête tous les serveurs d'essai et supprime le dossier temporaire.
cleanup() {
    for pid in "${SERVER_PIDS[@]}"; do
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    done
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

# Affiche PASS ou FAIL selon le résultat d'une commande et compte.
# Paramètres : $1 = nom du test, la suite = commande à exécuter.
check() {
    local name="$1"
    shift
    if "$@"; then
        echo "PASS : $name"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "FAIL : $name"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# Lance le scanner et vérifie son code de sortie.
# Paramètres : $1 = nom du test, $2 = code attendu, la suite = arguments.
expect_code() {
    local name="$1"
    local expected="$2"
    shift 2
    "$BIN" "$@" > /dev/null 2>&1
    local code=$?
    check "$name (code $code, attendu $expected)" test "$code" -eq "$expected"
}

# Indique si un port de 127.0.0.1 accepte une connexion TCP.
# Paramètre : $1 = port.
port_is_open() {
    (exec 3<>"/dev/tcp/$TARGET/$1") 2>/dev/null
}

# Attend (au plus 5 s) qu'un serveur d'essai écoute sur un port.
# Paramètre : $1 = port.
wait_for_port() {
    for _ in $(seq 1 50); do
        port_is_open "$1" && return 0
        sleep 0.1
    done
    echo "Le serveur d'essai du port $1 n'a pas démarré." >&2
    return 1
}

# Démarre un serveur d'essai en arrière-plan et retient son PID.
# Paramètres : les arguments de la commande à lancer.
start_server() {
    "$@" > /dev/null 2>&1 &
    SERVER_PIDS+=("$!")
}

# Cherche un port fermé, en commençant par 9999.
find_closed_port() {
    for port in 9999 9998 9997 9996 9995; do
        if ! port_is_open "$port"; then
            echo "$port"
            return 0
        fi
    done
    return 1
}

if [ ! -x "$BIN" ]; then
    echo "Programme $BIN absent : lancez make d'abord." >&2
    exit 1
fi
for port in "$HTTP_PORT" "$BANNER_PORT" "$HTML_PORT" $(seq "$SILENT_FIRST" "$SILENT_LAST"); do
    if port_is_open "$port"; then
        echo "Le port $port est déjà utilisé : tests impossibles." >&2
        exit 1
    fi
done
CLOSED_PORT="$(find_closed_port)" || { echo "Aucun port fermé trouvé." >&2; exit 1; }

echo "== Démarrage des serveurs d'essai sur $TARGET =="
mkdir -p "$WORK_DIR/www"
start_server python3 -m http.server "$HTTP_PORT" --bind "$TARGET" --directory "$WORK_DIR/www"
start_server python3 -c "$SERVER_CODE" banner "$BANNER_PORT" "TEST-BANNER-OK"
start_server python3 -c "$SERVER_CODE" banner "$HTML_PORT" "<b>gras</b> & 'test'"
start_server python3 -c "$SERVER_CODE" silent $(seq "$SILENT_FIRST" "$SILENT_LAST")
for port in "$HTTP_PORT" "$BANNER_PORT" "$HTML_PORT" "$SILENT_FIRST"; do
    wait_for_port "$port" || exit 1
done
PORTS="$HTTP_PORT,$BANNER_PORT,$HTML_PORT,$CLOSED_PORT"
echo "Ports testés : $PORTS (port fermé choisi : $CLOSED_PORT)"

echo "== Détection des ports et bannières =="
OUT="$WORK_DIR/scan.txt"
"$BIN" "$TARGET" -p "$PORTS" -v > "$OUT" 2> "$WORK_DIR/scan_err.txt"
check "scan avec -v : code de sortie 0" test $? -eq 0
check "$HTTP_PORT détecté ouvert" grep -Eq "^$HTTP_PORT/tcp +ouvert" "$OUT"
check "$BANNER_PORT détecté ouvert" grep -Eq "^$BANNER_PORT/tcp +ouvert" "$OUT"
check "$CLOSED_PORT détecté fermé (avec -v)" grep -Eq "^$CLOSED_PORT/tcp +fermé" "$OUT"
check "bannière TEST-BANNER-OK sur $BANNER_PORT" grep -Eq "^$BANNER_PORT/tcp .*TEST-BANNER-OK" "$OUT"
check "bannière HTTP sur $HTTP_PORT (requête HEAD)" grep -Eq "^$HTTP_PORT/tcp .*HTTP/1\.[01] 200" "$OUT"
check "service http-alt sur $HTTP_PORT" grep -Eq "^$HTTP_PORT/tcp +ouvert +http-alt" "$OUT"
check "résumé : 3 ouverts, 1 fermés" grep -q "Résumé : 3 ouverts, 1 fermés, 0 filtrés" "$OUT"
check "avertissement légal sur la sortie d'erreur" grep -q "^ATTENTION : à utiliser uniquement" "$WORK_DIR/scan_err.txt"
"$BIN" "$TARGET" -p "$PORTS" > "$WORK_DIR/quiet.txt" 2> /dev/null
check "sans -v : le port fermé n'est pas listé" bash -c "! grep -q '^$CLOSED_PORT/tcp' '$WORK_DIR/quiet.txt'"

echo "== Rapports =="
TEXT_REPORT="$WORK_DIR/rapport.txt"
HTML_REPORT="$WORK_DIR/rapport.html"
"$BIN" "$TARGET" -p "$PORTS" -o "$TEXT_REPORT" > /dev/null 2>&1
check "rapport texte : code de sortie 0" test $? -eq 0
check "rapport texte créé" test -s "$TEXT_REPORT"
check "rapport texte : date du scan" grep -Eq "^Date du scan : [0-9]{4}-[0-9]{2}-[0-9]{2} " "$TEXT_REPORT"
check "rapport texte : bannière TEST-BANNER-OK" grep -q "TEST-BANNER-OK" "$TEXT_REPORT"
check "rapport texte : résumé" grep -q "^Résumé : 3 ouverts" "$TEXT_REPORT"
"$BIN" "$TARGET" -p "$PORTS" -o "$HTML_REPORT" > /dev/null 2>&1
check "rapport HTML : code de sortie 0" test $? -eq 0
check "rapport HTML créé avec doctype" grep -q "^<!DOCTYPE html>" "$HTML_REPORT"
check "rapport HTML : meta charset UTF-8" grep -q '<meta charset="UTF-8">' "$HTML_REPORT"
check "rapport HTML : port $BANNER_PORT et sa bannière" grep -q "<td>$BANNER_PORT/tcp</td>.*TEST-BANNER-OK" "$HTML_REPORT"
check "rapport HTML : bannière <b> échappée" grep -q "&lt;b&gt;gras&lt;/b&gt; &amp; &#39;test&#39;" "$HTML_REPORT"
check "rapport HTML : aucune balise <b> venue du réseau" bash -c "! grep -q '<b>gras' '$HTML_REPORT'"
check "rapport HTML : port fermé absent du tableau" bash -c "! grep -q '<td>$CLOSED_PORT/tcp' '$HTML_REPORT'"
check "rapport HTML : page complète" grep -q "^</html>" "$HTML_REPORT"

echo "== Erreurs d'arguments et codes de sortie =="
expect_code "aide -h" 0 -h
expect_code "port 0" 1 "$TARGET" -p 0
expect_code "port 70000" 1 "$TARGET" -p 70000
expect_code "plage inversée 100-50" 1 "$TARGET" -p 100-50
expect_code "liste avec virgule en trop" 1 "$TARGET" -p 80,
expect_code "port non numérique" 1 "$TARGET" -p abc
expect_code "timeout négatif" 1 "$TARGET" -t -5
expect_code "timeout trop petit" 1 "$TARGET" -t 10
expect_code "timeout non numérique" 1 "$TARGET" -t 12ms
expect_code "cible absente" 1 -p 80
expect_code "deux cibles" 1 "$TARGET" "$TARGET"
expect_code "option inconnue -z" 1 "$TARGET" -z
expect_code "valeur manquante pour -p" 1 "$TARGET" -p
expect_code "cible inexistante nom-invalide.invalid" 2 nom-invalide.invalid -p 80
expect_code "rapport impossible à écrire /proc/interdit.txt" 3 "$TARGET" -p "$BANNER_PORT" -o /proc/interdit.txt
if [ -w /dev/full ]; then
    expect_code "rapport sur un disque plein (/dev/full)" 3 "$TARGET" -p "$BANNER_PORT" -o /dev/full
fi

echo "== Délai très court =="
SHORT="$WORK_DIR/short.txt"
"$BIN" "$TARGET" -p "$PORTS" -t 50 -v > "$SHORT" 2> /dev/null
check "timeout 50 ms : code de sortie 0" test $? -eq 0
check "timeout 50 ms : $BANNER_PORT toujours ouvert" grep -Eq "^$BANNER_PORT/tcp +ouvert" "$SHORT"
check "timeout 50 ms : $CLOSED_PORT toujours fermé" grep -Eq "^$CLOSED_PORT/tcp +fermé" "$SHORT"
check "timeout 50 ms : 4 ports classés" grep -q "Résumé : 3 ouverts, 1 fermés, 0 filtrés" "$SHORT"

echo "== Interruption par Ctrl+C (SIGINT) =="
# Lance un scan des ports muets (300 ms d'attente de bannière par port),
# envoie SIGINT au bout d'une seconde et vérifie l'arrêt propre.
# Paramètres : $1 = nom du test, la suite = options en plus.
test_sigint() {
    local name="$1"
    shift
    local output="$WORK_DIR/interrupted.txt"
    "$BIN" "$TARGET" -p "$SILENT_FIRST-$SILENT_LAST" -v "$@" > "$output" 2> /dev/null &
    local scan_pid=$!
    sleep 1
    kill -INT "$scan_pid"
    wait "$scan_pid"
    local code=$?
    check "$name : code de sortie 130 (obtenu $code)" test "$code" -eq 130
    check "$name : mention scan interrompu" grep -q "scan interrompu" "$output"
    check "$name : résumé partiel affiché" grep -q "^Résumé : " "$output"
}
test_sigint "SIGINT"
test_sigint "SIGINT avec -j 4" -j 4

echo "== Option -j (threads) =="
expect_code "-j 0 refusé" 1 "$TARGET" -j 0
expect_code "-j 65 refusé" 1 "$TARGET" -j 65
expect_code "-j abc refusé" 1 "$TARGET" -j abc
expect_code "-j 64 accepté" 0 "$TARGET" -p "$PORTS" -j 64
# Scan de référence avec un seul thread (la ligne de durée est retirée).
"$BIN" "$TARGET" -p "1-1024,$PORTS" -v -j 1 2> /dev/null | sed 's/ - durée : .*//' > "$WORK_DIR/ref.txt"
check "-j 1 : la référence contient $BANNER_PORT ouvert" grep -Eq "^$BANNER_PORT/tcp +ouvert" "$WORK_DIR/ref.txt"
SAME=0
for run in $(seq 1 "$THREAD_RUNS"); do
    "$BIN" "$TARGET" -p "1-1024,$PORTS" -v -j 8 2> /dev/null | sed 's/ - durée : .*//' > "$WORK_DIR/j8.txt"
    if cmp -s "$WORK_DIR/ref.txt" "$WORK_DIR/j8.txt"; then
        SAME=$((SAME + 1))
    else
        diff "$WORK_DIR/ref.txt" "$WORK_DIR/j8.txt" | head -5
    fi
done
check "-j 8 : $SAME/$THREAD_RUNS scans identiques à -j 1" test "$SAME" -eq "$THREAD_RUNS"

echo "== Bilan =="
echo "Tests de bout en bout : $PASS_COUNT réussis, $FAIL_COUNT échoués"
if [ "$FAIL_COUNT" -ne 0 ]; then
    exit 1
fi
exit 0
