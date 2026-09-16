# SO2026B — Shell educativo y editor de texto POSIX

Repositorio del Parcial 1 de Sistemas Operativos.

Equipo:

Shell base de: https://github.com/evalenciEAFIT/SO2026B/tree/main

## Subproyectos

### `shell/`

Contiene el shell educativo `sys_shell` y la implementación del parcial del editor de texto `cat_editor`.

Los puntos principales del parcial están en:

- `shell/cat_editor.c`: editor de texto y estructura `Line -> Word`.
- `shell/cat_editor_launch.c`: integración mediante `fork()`, `execv()` y `waitpid()`.
- `shell/main.c`: dispatcher del shell y categoría `editor`.
- `shell/shell.h`: estructuras y prototipos compartidos.
- `shell/tests/test_cat_editor.c`: suite automática POSIX escrita en C.
- `shell/Makefile`: compilación y pruebas.

Compilar y probar desde `shell/`:

```bash
make clean
make
make test
```

El proyecto requiere un entorno Linux/POSIX. La suite de pruebas usa `forkpty()` para validar el editor de pantalla completa.

### `Banderas_CLI/`

Subproyecto independiente para estudiar el parseo de argumentos y opciones en C mediante parseo manual, `getopt()` y `getopt_long()`.
