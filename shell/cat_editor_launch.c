#include "shell.h"
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/**
 * ====================================================================================
 * COMANDO: e_open <archivo>
 * ====================================================================================
 * CATEGORÍA NUEVA: "editor"
 *
 * Justificación arquitectónica (ver INTEGRACION_SHELL.md para el análisis
 * completo): Las categorías existentes ("datos", "memoria", "monitoreo",
 * "utilidades") están diseñadas para DEMOSTRACIONES ATÓMICAS: el usuario
 * ejecuta el comando, el shell imprime la(s) syscall(s) involucradas y el
 * control vuelve inmediatamente al prompt "eafitOS>". cat_editor rompe ese
 * contrato: es un programa interactivo de ciclo propio (su propio REPL) que
 * toma el control de STDIN/STDOUT hasta que el usuario escribe 'q'. Meterlo
 * dentro de "datos" (por compartir syscalls de I/O de bajo nivel con
 * d_read/d_create) mezclaría dos tipos de comandos con contratos de ejecución
 * distintos en la misma categoría educativa. Por eso se crea la categoría
 * "editor", análoga en mecanismo a "monitoreo" (que ya demuestra cómo un shell
 * delega el control a un programa externo con p_exec) pero conceptualmente
 * separada porque su propósito no es "monitorear procesos" sino "editar
 * archivos".
 *
 * Mecanismo de invocación:
 * Igual que p_exec, NO se reimplementa el editor dentro del proceso del shell.
 * Se hace fork() y en el hijo se reemplaza la imagen de memoria con execv()
 * sobre el binario ya compilado "./cat_editor", pasándole el archivo como
 * argv[1]. El padre (el shell) se bloquea con waitpid() hasta que el usuario
 * cierra el editor con 'q', y entonces el control regresa al prompt "eafitOS>".
 *
 * Esto es exactamente cómo bash, zsh, etc. lanzan editores como vim o nano:
 * el shell no "sabe" editar texto, solo sabe crear procesos y cederles la
 * terminal.
 *
 * Syscalls explicadas:
 * 1. fork(2): Clona el proceso del shell.
 * 2. execv(3): Reemplaza la imagen del proceso hijo por el binario cat_editor.
 * 3. waitpid(2): El shell padre espera a que el editor termine (comando 'q').
 */
int cmd_e_open(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, COLOR_ERROR "Uso: e_open <archivo>\n" COLOR_RESET);
    return 1;
  }

  /* 1. LLAMADA AL SISTEMA: fork */
  LOG_SYSCALL("fork", "");
  pid_t pid = fork();
  if (pid == -1) {
    LOG_SYSCALL_ERROR(strerror(errno));
    return 1;
  }

  if (pid == 0) {
    /* PROCESO HIJO: se convierte en cat_editor */
    char *exec_args[] = {"./cat_editor", argv[1], NULL};

    LOG_SYSCALL("execv", "\"./cat_editor\", argv");
    printf("\n" COLOR_INFO "[Hijo] Cediendo la terminal a cat_editor sobre "
                           "'%s'...\n\n" COLOR_RESET,
           argv[1]);
    /* Como stdout puede estar completamente bufferizado cuando el shell se
     * ejecuta con una tuberia, forzamos la salida antes de execv(). */
    fflush(stdout);

    execv("./cat_editor", exec_args);

    /* Si execv retorna, hubo un error (e.g. binario no compilado/no encontrado)
     */
    LOG_SYSCALL_ERROR(strerror(errno));
    fprintf(stderr, COLOR_ERROR
            "Error: no se pudo ejecutar './cat_editor'. "
            "¿Compilaste el editor con 'make cat_editor'?\n" COLOR_RESET);
    exit(127);
  }

  /* PROCESO PADRE (el shell) */
  LOG_SYSCALL_RESULT(pid);
  printf(
      COLOR_PROMPT
      "[Padre]" COLOR_RESET
      " Editor lanzado (PID %d). Esperando a que el usuario escriba 'q'...\n",
      pid);

  int status;
  LOG_SYSCALL("waitpid", "%d, &status, 0", pid);
  pid_t waited_pid = waitpid(pid, &status, 0);
  if (waited_pid == -1) {
    LOG_SYSCALL_ERROR(strerror(errno));
    return 1;
  }
  LOG_SYSCALL_RESULT(waited_pid);

  if (WIFEXITED(status)) {
    printf(COLOR_PROMPT
           "[Padre]" COLOR_RESET
           " cat_editor terminó normalmente. Código de salida: " COLOR_RESULT
           "%d" COLOR_RESET "\n",
           WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    printf(COLOR_PROMPT "[Padre]" COLOR_RESET
                        " cat_editor fue terminado por una señal: " COLOR_ERROR
                        "%d" COLOR_RESET "\n",
           WTERMSIG(status));
  }

  return 0;
}
