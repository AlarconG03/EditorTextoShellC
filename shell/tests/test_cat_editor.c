/*
 * test_cat_editor.c
 * ---------------------------------------------------------------------------
 * Suite de pruebas POSIX para cat_editor (SO2026B).
 *
 * Se ejecuta en Linux y evita depender de Python/pty. Comprueba:
 *   1) Restriccion critica de I/O en cat_editor.c.
 *   2) Comandos acumulativos del equipo de 3.
 *   3) Casos borde y manejo de errores.
 *   4) Editor libre y atajos Alt+S/Q/E/G mediante una pseudo-terminal.
 *
 * El ejecutable de pruebas NO forma parte del programa entregable del editor;
 * es un programa auxiliar para demostrar y repetir las validaciones.
 */

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define TEST_TIMEOUT 10
#define OUTPUT_LIMIT (1024 * 1024)

static char g_root[4096];

static void fail_test(const char *message) {
    fprintf(stderr, "[FAIL] %s\n", message);
    exit(EXIT_FAILURE);
}

static void ok_test(const char *message) {
    printf("[ OK ] %s\n", message);
}

static void join_path(char *out, size_t out_size, const char *name) {
    int n = snprintf(out, out_size, "%s/%s", g_root, name);
    if (n < 0 || (size_t)n >= out_size) fail_test("Ruta de prueba demasiado larga.");
}

static int read_file_all(const char *path, char **out, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) return 0;

    if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
        close(fd);
        return 0;
    }

    size_t cap = 256;
    char *buf = malloc(cap);
    if (!buf) {
        close(fd);
        return 0;
    }

    size_t len = 0;
    for (;;) {
        if (len + 1 >= cap) {
            size_t next = cap * 2;
            char *tmp = realloc(buf, next);
            if (!tmp) {
                free(buf);
                close(fd);
                return 0;
            }
            buf = tmp;
            cap = next;
        }

        ssize_t r = read(fd, buf + len, cap - len - 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf);
            close(fd);
            return 0;
        }
        if (r == 0) break;
        len += (size_t)r;
    }

    if (close(fd) == -1) {
        free(buf);
        return 0;
    }

    buf[len] = '\0';
    *out = buf;
    if (out_len) *out_len = len;
    return 1;
}

static int write_file_all(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return 0;

    size_t len = strlen(text);
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, text + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return 0;
        }
        if (n == 0) {
            close(fd);
            return 0;
        }
        done += (size_t)n;
    }

    return close(fd) == 0;
}

static void make_test_file(char *path, size_t path_size, const char *name,
                           const char *content) {
    join_path(path, path_size, name);
    if (!write_file_all(path, content)) {
        perror("write_file_all");
        fail_test("No se pudo crear el archivo temporal de prueba.");
    }
}

static void remove_test_file(const char *path) {
    unlink(path);
}

/* Espera al hijo con un timeout para evitar que una prueba colgada bloquee toda
 * la suite. El hijo se termina con SIGKILL si supera el tiempo permitido. */
static int wait_with_timeout(pid_t pid, int seconds, int *status_out) {
    for (int i = 0; i < seconds * 10; i++) {
        pid_t r = waitpid(pid, status_out, WNOHANG);
        if (r == pid) return 1;
        if (r == -1) return 0;
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, status_out, 0);
    return 0;
}

static int run_with_pipes(const char *program, char *const argv[],
                          const char *input, char *output, size_t out_size,
                          int *exit_status) {
    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) == -1 || pipe(out_pipe) == -1) return 0;

    pid_t pid = fork();
    if (pid == -1) return 0;

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        alarm(TEST_TIMEOUT);
        execv(program, argv);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);

    size_t input_len = strlen(input);
    size_t sent = 0;
    while (sent < input_len) {
        ssize_t n = write(in_pipe[1], input + sent, input_len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(in_pipe[1]);
            close(out_pipe[0]);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            return 0;
        }
        if (n == 0) break;
        sent += (size_t)n;
    }
    close(in_pipe[1]);

    size_t total = 0;
    while (total + 1 < out_size) {
        ssize_t n = read(out_pipe[0], output + total, out_size - total - 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(out_pipe[0]);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            return 0;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    output[total] = '\0';
    close(out_pipe[0]);

    int status = 0;
    int finished = wait_with_timeout(pid, TEST_TIMEOUT, &status);
    if (exit_status) *exit_status = status;
    return finished;
}

static int contains(const char *text, const char *needle) {
    return strstr(text, needle) != NULL;
}

static void test_static_io(void) {
    char source_path[4096];
    join_path(source_path, sizeof(source_path), "cat_editor.c");

    char *source = NULL;
    size_t source_len = 0;
    if (!read_file_all(source_path, &source, &source_len)) {
        (void)source_len;
        fail_test("No se pudo leer cat_editor.c para la revision estatica.");
    }

    const char *forbidden[] = {
        "fopen(", "fread(", "fwrite(", "fclose(",
        "read(STDIN_FILENO"
    };
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        if (contains(source, forbidden[i])) {
            free(source);
            fail_test("Se encontro una operacion de I/O no permitida en cat_editor.c.");
        }
    }

    const char *required[] = {
        "open(", "read(", "write(", "lseek(", "ftruncate(", "close(",
        "fstat(", "scanf(\"%c\""
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (!contains(source, required[i])) {
            free(source);
            fail_test("Falta una evidencia requerida de la estrategia de I/O en cat_editor.c.");
        }
    }

    free(source);
    ok_test("Restriccion critica de I/O y uso de syscalls requeridas");
}

static void test_command_suite(void) {
    char path[4096];
    make_test_file(path, sizeof(path), "commands.txt", "Uno\nDos\nTres\n");

    char editor[4096];
    join_path(editor, sizeof(editor), "cat_editor");
    char *argv[] = {editor, path, NULL};

    const char *input =
        "a Cuatro\n"
        "i 2 Insertado\n"
        "s Dos\n"
        "y 2\n"
        "x 5\n"
        "m\n"
        "p 1\n"
        "p 5\n"
        "d 3\n"
        "q\n";

    char output[OUTPUT_LIMIT];
    int status = 0;
    if (!run_with_pipes(editor, argv, input, output, sizeof(output), &status)) {
        remove_test_file(path);
        fail_test("El proceso de cat_editor no termino durante la prueba funcional.");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        remove_test_file(path);
        fail_test("cat_editor termino con un codigo distinto de 0 en la prueba funcional.");
    }

    char *final = NULL;
    if (!read_file_all(path, &final, NULL)) {
        remove_test_file(path);
        fail_test("No se pudo leer el archivo final de la prueba funcional.");
    }

    const char *expected = "Uno\nInsertado\nTres\nInsertado\nCuatro\n";
    if (strcmp(final, expected) != 0) {
        free(final);
        remove_test_file(path);
        fail_test("El contenido final no coincide con el resultado esperado de a/i/s/y/x/m/p/d.");
    }
    free(final);

    const char *messages[] = {
        "Linea agregada al final",
        "Texto insertado en la linea 2",
        "3: Dos",
        "Linea 2 copiada al portapapeles",
        "Portapapeles pegado en la linea 5",
        "Metadata",
        "Uno",
        "Insertado",
        "Linea 3 eliminada"
    };
    for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); i++) {
        if (!contains(output, messages[i])) {
            remove_test_file(path);
            fail_test("Falto una evidencia de comando en la salida de la prueba funcional.");
        }
    }

    remove_test_file(path);
    ok_test("Comandos acumulativos del equipo de 3: base + i + s + m + y/x");
}

static void test_error_suite(void) {
    char path[4096];
    make_test_file(path, sizeof(path), "errors.txt", "Linea uno\n");

    char editor[4096];
    join_path(editor, sizeof(editor), "cat_editor");
    char *argv[] = {editor, path, NULL};

    const char *input =
        "p 99\n"
        "d 99\n"
        "i 99 fuera\n"
        "y 99\n"
        "x 99\n"
        "q\n";

    char output[OUTPUT_LIMIT];
    int status = 0;
    if (!run_with_pipes(editor, argv, input, output, sizeof(output), &status)) {
        remove_test_file(path);
        fail_test("La prueba de errores dejo el editor bloqueado.");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        remove_test_file(path);
        fail_test("cat_editor termino con error inesperado durante los casos borde.");
    }

    if (!contains(output, "Error: la linea 99 no existe.")) {
        remove_test_file(path);
        fail_test("No se reporto correctamente la linea inexistente.");
    }
    if (!contains(output, "Error: no se puede insertar en la linea 99")) {
        remove_test_file(path);
        fail_test("No se reporto correctamente la insercion fuera de rango.");
    }

    char *final = NULL;
    if (!read_file_all(path, &final, NULL)) {
        remove_test_file(path);
        fail_test("No se pudo leer el archivo de la prueba de errores.");
    }
    if (strcmp(final, "Linea uno\n") != 0) {
        free(final);
        remove_test_file(path);
        fail_test("Un comando invalido modifico el archivo.");
    }
    free(final);
    remove_test_file(path);
    ok_test("Manejo de errores y casos borde sin modificar el archivo");
}

static int read_until(int fd, const char *needle, char *buffer, size_t cap,
                      int timeout_seconds) {
    size_t len = strlen(buffer);
    ;
    struct timeval tv;
    fd_set set;
    int loops = timeout_seconds * 20;

    for (int i = 0; i < loops; i++) {
        FD_ZERO(&set);
        FD_SET(fd, &set);
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        int ready = select(fd + 1, &set, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (ready == 0) continue;

        if (len + 1 >= cap) return 0;
        ssize_t n = read(fd, buffer + len, cap - len - 1);
        if (n <= 0) return 0;
        len += (size_t)n;
        buffer[len] = '\0';
        if (strstr(buffer, needle) != NULL) return 1;
    }
    return 0;
}

static void test_interactive_suite(void) {
    char path[4096];
    make_test_file(path, sizeof(path), "interactive.txt", "Hola\nMundo\n");

    char editor[4096];
    join_path(editor, sizeof(editor), "cat_editor");

    int master = -1;
    struct winsize ws = {.ws_row = 30, .ws_col = 120, .ws_xpixel = 0, .ws_ypixel = 0};
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid == -1) {
        remove_test_file(path);
        fail_test("forkpty no esta disponible en el entorno Linux.");
    }

    if (pid == 0) {
        execl(editor, editor, path, (char *)NULL);
        _exit(127);
    }

    char output[OUTPUT_LIMIT] = {0};
    if (!read_until(master, "cat_editor:", output, sizeof(output), TEST_TIMEOUT)) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(master);
        remove_test_file(path);
        fail_test("No aparecio el prompt inicial del editor.");
    }

    const char *enter_free_editor = "e\n";
    output[0] = '\0';
    write(master, enter_free_editor, strlen(enter_free_editor));
    if (!read_until(master, "CAT_EDITOR", output, sizeof(output), TEST_TIMEOUT)) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(master);
        remove_test_file(path);
        fail_test("No se pudo entrar al editor libre.");
    }

    /* Escribe X al inicio, retrocede una posicion y escribe Y: YXHola. */
    write(master, "X", 1);
    write(master, "\x1b[D", 3);
    write(master, "Y", 1);

    /* Alt+S -> guardar; Alt+E -> estructura; Alt+E -> volver; Alt+G -> ayuda;
     * Alt+G -> volver; Alt+Q -> guardar y salir al REPL. */
    write(master, "\033" "s", 2);
    usleep(100000);
    write(master, "\033" "e", 2);
    output[0] = '\0';
    if (!read_until(master, "ESTRUCTURA DE DATOS", output, sizeof(output), TEST_TIMEOUT)) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(master);
        remove_test_file(path);
        fail_test("Alt+E no abrio la estructura de datos.");
    }
    write(master, "\033" "e", 2);
    usleep(100000);
    write(master, "\033" "g", 2);
    output[0] = '\0';
    if (!read_until(master, "CAT_EDITOR - AYUDA", output, sizeof(output), TEST_TIMEOUT)) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(master);
        remove_test_file(path);
        fail_test("Alt+G no abrio la ayuda.");
    }
    write(master, "\033" "g", 2);
    usleep(100000);
    write(master, "\033" "q", 2);
    output[0] = '\0';

    if (!read_until(master, "cat_editor:", output, sizeof(output), TEST_TIMEOUT)) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(master);
        remove_test_file(path);
        fail_test("Alt+Q no devolvio el control al modo de comandos.");
    }

    write(master, "q\n", 2);
    int status = 0;
    if (!wait_with_timeout(pid, TEST_TIMEOUT, &status)) {
        close(master);
        remove_test_file(path);
        fail_test("El editor libre no termino a tiempo.");
    }
    close(master);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        remove_test_file(path);
        fail_test("cat_editor termino con error despues de la prueba interactiva.");
    }

    char *final = NULL;
    if (!read_file_all(path, &final, NULL)) {
        remove_test_file(path);
        fail_test("No se pudo leer el archivo final del editor libre.");
    }
    if (strcmp(final, "YXHola\nMundo\n") != 0) {
        free(final);
        remove_test_file(path);
        fail_test("La navegacion/insercion del editor libre produjo contenido incorrecto.");
    }
    free(final);
    remove_test_file(path);
    ok_test("Editor libre: Alt+S/Q/E/G, flechas y retorno al modo de comandos");
}

static void prepare_root(const char *argv0) {
    char resolved[4096];
    if (!realpath(argv0, resolved)) {
        perror("realpath");
        fail_test("No se pudo resolver la ruta del ejecutable de pruebas.");
    }

    char *slash = strrchr(resolved, '/');
    if (!slash) fail_test("No se pudo determinar el directorio del ejecutable.");
    *slash = '\0'; /* .../shell/tests */
    slash = strrchr(resolved, '/');
    if (!slash) fail_test("No se pudo determinar el directorio shell.");
    *slash = '\0'; /* .../shell */
    strncpy(g_root, resolved, sizeof(g_root) - 1);
    g_root[sizeof(g_root) - 1] = '\0';
}

int main(int argc, char **argv) {
    (void)argc;
    prepare_root(argv[0]);

    printf("\n=== PRUEBAS AUTOMATICAS DE cat_editor (SO2026B) ===\n");
    printf("Directorio del proyecto: %s\n\n", g_root);

    test_static_io();
    test_command_suite();
    test_error_suite();
    test_interactive_suite();

    printf("\nTodas las pruebas pasaron correctamente.\n");
    return EXIT_SUCCESS;
}
