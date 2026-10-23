# Fichier : Makefile
# Ce fichier sert à compiler PortScan-C (compilation séparée en fichiers .o
# dans le dossier build/) et à lancer les tests et valgrind.

# Compilateur utilisé.
CC = gcc
# Options de compilation : C11 strict, tous les avertissements traités
# comme des erreurs, optimisation, fonctions POSIX 2008, threads POSIX,
# dossier include/.
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -D_POSIX_C_SOURCE=200809L -pthread -Iinclude
# Options de l'édition de liens (bibliothèque des threads POSIX).
LDFLAGS = -pthread

# Nom du programme final.
TARGET = portscan
# Dossier qui reçoit les fichiers objets (.o).
BUILD_DIR = build
# Fichiers objets du programme.
OBJECTS = $(BUILD_DIR)/main.o $(BUILD_DIR)/ports.o $(BUILD_DIR)/scanner.o \
          $(BUILD_DIR)/report.o $(BUILD_DIR)/utils.o
# Programme de tests unitaires.
TEST_BIN = $(BUILD_DIR)/test_units
# Commande valgrind : toutes les fuites, code 1 si une erreur est trouvée.
VALGRIND = valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1

.PHONY: all test clean valgrind

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@ $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Chaque .o dépend de son .c et des en-têtes qu'il inclut.
$(BUILD_DIR)/main.o: src/main.c include/ports.h include/report.h include/scanner.h include/utils.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ports.o: src/ports.c include/ports.h include/utils.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/scanner.o: src/scanner.c include/scanner.h include/utils.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/report.o: src/report.c include/report.h include/scanner.h include/utils.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/utils.o: src/utils.c include/utils.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_units.o: tests/test_units.c include/ports.h include/utils.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_BIN): $(BUILD_DIR)/test_units.o $(BUILD_DIR)/ports.o $(BUILD_DIR)/utils.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Tests unitaires puis tests de bout en bout (sur 127.0.0.1 uniquement).
test: $(TARGET) $(TEST_BIN)
	./$(TEST_BIN)
	bash tests/run_tests.sh

# Scan de localhost sous valgrind : 0 erreur et 0 fuite attendues.
valgrind: $(TARGET)
	$(VALGRIND) ./$(TARGET) 127.0.0.1 -p 1-1024 -t 200 -v -o $(BUILD_DIR)/valgrind_report.html > /dev/null

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
