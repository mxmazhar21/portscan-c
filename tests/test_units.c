/*
 * Fichier : test_units.c
 * Ce fichier sert à tester, avec assert, les fonctions qui ne touchent pas
 * au réseau : décodage de la liste de ports (ports.c), nettoyage des
 * bannières et échappement HTML (utils.c). Si un test échoue, assert arrête
 * le programme avec un message qui donne la ligne en cause.
 */
#include "ports.h"
#include "utils.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Taille des tampons de sortie utilisés dans les tests. */
#define TEST_BUFFER_SIZE 256
/* Taille d'une bannière "trop longue" (plus grande que le tampon). */
#define LONG_INPUT_SIZE 600

/* Nombre de tests réussis, affiché à la fin. */
static int passed_tests = 0;

/*
 * Affiche le nom d'un test réussi et le compte.
 * Paramètre : name = nom du test.
 */
static void report_pass(const char *name)
{
    passed_tests++;
    printf("PASS : %s\n", name);
}

/*
 * Vérifie qu'un texte donne exactement la liste de ports attendue.
 * Paramètres : text = texte de l'option -p, expected/count = ports attendus.
 */
static void expect_ports(const char *text, const int *expected, size_t count)
{
    port_list_t list;

    assert(parse_port_list(text, &list) == PORTS_OK);
    assert(list.count == count);
    for (size_t i = 0; i < count; i++) {
        assert(list.ports[i] == expected[i]);
    }
    free_port_list(&list);
}

/*
 * Vérifie qu'un texte est refusé avec le code d'erreur attendu.
 * Paramètres : text = texte de l'option -p, expected = code attendu.
 */
static void expect_port_error(const char *text, port_error_t expected)
{
    port_list_t list;

    assert(parse_port_list(text, &list) == expected);
    assert(list.ports == NULL && list.count == 0);
}

/* Teste les formats valides : port seul, plage, liste, mélange. */
static void test_valid_port_lists(void)
{
    expect_ports("80", (const int[]){ 80 }, 1);
    report_pass("port seul \"80\"");
    expect_ports("20-23", (const int[]){ 20, 21, 22, 23 }, 4);
    report_pass("plage \"20-23\"");
    expect_ports("22,80,443", (const int[]){ 22, 80, 443 }, 3);
    report_pass("liste \"22,80,443\"");
    expect_ports("1-3,443,8000-8001", (const int[]){ 1, 2, 3, 443, 8000, 8001 }, 6);
    report_pass("mélange \"1-3,443,8000-8001\"");
    expect_ports("80,80,79-81,80", (const int[]){ 79, 80, 81 }, 3);
    report_pass("doublons éliminés");
    expect_ports("443,22,80", (const int[]){ 22, 80, 443 }, 3);
    report_pass("ports triés");
}

/* Teste la plage complète 1-65535 (premier, dernier, nombre de ports). */
static void test_full_range(void)
{
    port_list_t list;

    assert(parse_port_list("1-65535", &list) == PORTS_OK);
    assert(list.count == 65535);
    assert(list.ports[0] == 1 && list.ports[65534] == 65535);
    free_port_list(&list);
    report_pass("plage complète \"1-65535\"");
}

/* Teste les textes invalides : bornes, signe, lettres, vide, virgule. */
static void test_invalid_port_lists(void)
{
    expect_port_error("0", PORTS_ERROR_RANGE);
    report_pass("refus de \"0\"");
    expect_port_error("65536", PORTS_ERROR_RANGE);
    report_pass("refus de \"65536\"");
    expect_port_error("-5", PORTS_ERROR_SYNTAX);
    report_pass("refus de \"-5\"");
    expect_port_error("abc", PORTS_ERROR_SYNTAX);
    report_pass("refus de \"abc\"");
    expect_port_error("", PORTS_ERROR_SYNTAX);
    report_pass("refus de la liste vide");
    expect_port_error("80,", PORTS_ERROR_SYNTAX);
    report_pass("refus de \"80,\"");
    expect_port_error("100-50", PORTS_ERROR_ORDER);
    report_pass("refus de la plage inversée \"100-50\"");
    expect_port_error("99999999999999999999", PORTS_ERROR_RANGE);
    report_pass("refus d'un nombre géant");
}

/* Teste parse_long_in_range (utilisée pour -t et -j). */
static void test_parse_long(void)
{
    long value = 0;

    assert(parse_long_in_range("500", 50, 10000, &value) == 0 && value == 500);
    assert(parse_long_in_range("49", 50, 10000, &value) != 0);
    assert(parse_long_in_range("-5", 50, 10000, &value) != 0);
    assert(parse_long_in_range("12ms", 50, 10000, &value) != 0);
    assert(parse_long_in_range(" 60", 50, 10000, &value) != 0);
    report_pass("conversion de nombre avec bornes");
}

/* Teste le nettoyage des bannières. */
static void test_sanitize_banner(void)
{
    char output[TEST_BUFFER_SIZE];
    char long_input[LONG_INPUT_SIZE];
    const char raw[] = { 'A', '\t', 'B', '\x01', '\0', 'C', '\x7f', 'D' };

    sanitize_banner(raw, sizeof(raw), output, sizeof(output));
    assert(strcmp(output, "A.B..C.D") == 0);
    report_pass("bannière : caractères non imprimables remplacés");
    sanitize_banner("SSH-2.0-Test\r\nsuite", 19, output, sizeof(output));
    assert(strcmp(output, "SSH-2.0-Test") == 0);
    report_pass("bannière : seule la première ligne est gardée");
    sanitize_banner("", 0, output, sizeof(output));
    assert(output[0] == '\0');
    report_pass("bannière : chaîne vide");
    memset(long_input, 'x', sizeof(long_input));
    sanitize_banner(long_input, sizeof(long_input), output, sizeof(output));
    assert(strlen(output) == sizeof(output) - 1);
    report_pass("bannière : chaîne trop longue coupée sans débordement");
}

/* Teste l'échappement HTML. */
static void test_html_escape(void)
{
    char output[TEST_BUFFER_SIZE];
    char small[8];

    html_escape("&<>\"'", output, sizeof(output));
    assert(strcmp(output, "&amp;&lt;&gt;&quot;&#39;") == 0);
    report_pass("HTML : les 5 caractères spéciaux");
    html_escape("", output, sizeof(output));
    assert(output[0] == '\0');
    report_pass("HTML : chaîne vide");
    html_escape("<script>alert(1)</script>", output, sizeof(output));
    assert(strcmp(output, "&lt;script&gt;alert(1)&lt;/script&gt;") == 0);
    report_pass("HTML : \"<script>alert(1)</script>\"");
    html_escape("ab<cd", small, sizeof(small));
    assert(strcmp(small, "ab&lt;c") == 0);
    html_escape("abcd<", small, sizeof(small));
    assert(strcmp(small, "abcd") == 0);
    report_pass("HTML : tampon trop petit, entité jamais coupée");
}

/*
 * Lance tous les tests unitaires.
 * Retour : 0 si tous les tests passent (sinon assert arrête le programme).
 */
int main(void)
{
    test_valid_port_lists();
    test_full_range();
    test_invalid_port_lists();
    test_parse_long();
    test_sanitize_banner();
    test_html_escape();
    printf("Tests unitaires : %d réussis, 0 échoué\n", passed_tests);
    return 0;
}
