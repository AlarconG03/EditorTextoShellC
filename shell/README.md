# cat_editor + sys_shell — SO2026B

Proyecto del primer parcial de Sistemas Operativos (EAFIT): editor de texto interactivo en C para Linux/POSIX, integrado con el shell educativo `sys_shell`.

## Componentes principales

- `cat_editor.c`: editor de texto, estructura de datos y persistencia en disco.
- `cat_editor_launch.c`: comando `e_open`, que lanza `cat_editor` como proceso independiente con `fork()`, `execv()` y `waitpid()`.
- `main.c`: dispatcher y tabla de comandos del shell, incluida la nueva categoría `editor`.
- `shell.h`: estructuras, macros educativas y prototipos del shell.
- `tests/test_cat_editor.c`: pruebas automáticas POSIX escritas en C.
- `Makefile`: compilación, limpieza y ejecución de pruebas.

## Requisitos acumulativos del equipo de 3

Dentro de `cat_editor` se implementan los comandos base y los retos de pareja y trío:

| Comando | Función |
|---|---|
| `o [archivo]` | Abrir o crear el archivo con `open()` y cargarlo a memoria. |
| `p [n]` | Imprimir una línea o todo el documento. |
| `a [texto]` | Agregar una línea al final. |
| `d [n]` | Eliminar una línea. |
| `i [n] [texto]` | Insertar una línea en una posición arbitraria. |
| `s [palabra]` | Buscar texto en las líneas. |
| `m` | Mostrar tamaño, permisos, inodo y modificación mediante `fstat()`. |
| `y [n]` | Copiar una línea a un portapapeles local. |
| `x [n]` | Pegar una copia profunda del portapapeles. |
| `v` | Mostrar la lista ligada de líneas/palabras. |
| `e` | Abrir el editor libre de pantalla completa. |
| `q` | Cerrar el editor y liberar recursos. |

### Editor libre

El comando `e` permite mover el cursor con flechas, `Home`/`End`, insertar caracteres, partir líneas con `Enter`, borrar con `Backspace`/`Delete` y desplazarse cuando el contenido supera el área visible.

Los atajos de pantalla completa son:

```text
Alt+S  Guardar
Alt+Q  Guardar y salir del editor libre
Alt+E  Abrir/cerrar estructura de datos
Alt+G  Abrir/cerrar ayuda
```

Los mismos atajos aparecen en los pies de pantalla y en la ayuda interna para mantener la interfaz coherente.

## Restricción crítica de I/O

La manipulación del archivo de texto utiliza exclusivamente:

```text
open()
read()
write()
lseek()
ftruncate()
close()
```

La metadata requerida por el reto de equipo de 3 se obtiene con `fstat()`.

No se utilizan `fopen()`, `fread()`, `fwrite()` ni `fclose()` en `cat_editor.c`.

Para la interfaz del editor libre, la entrada de teclado se obtiene mediante `scanf("%c", ...)` en modo RAW. Las llamadas `read()` de `cat_editor` están reservadas para el descriptor del archivo de texto; no existe `read(STDIN_FILENO, ...)`.

## Estructura de datos

El documento reside en memoria como una lista doblemente enlazada de `Line`. Cada `Line` contiene una lista enlazada de `Word`; los espacios también se almacenan como nodos para poder reconstruir exactamente el texto.

El portapapeles usa una copia profunda de la lista de palabras para evitar compartir memoria con la línea original.

El editor libre usa buffers de caracteres independientes mientras se edita. Al guardar, esos buffers se sincronizan de nuevo con la estructura `Line -> Word` y luego se persisten en disco.

## Integración con `sys_shell`

La integración utiliza una categoría nueva llamada `editor`.

```text
sys_shell
   |
   | e_open archivo.txt
   v
 fork()
   |
   +---- padre ---- waitpid() ----> regreso al shell
   |
   +---- hijo ----- execv("./cat_editor", ...)
                           |
                           v
                     sesión interactiva
```

La categoría se separa de `datos` porque las operaciones de `datos` son atómicas, mientras que `cat_editor` mantiene una sesión interactiva completa con su propio ciclo de comandos.

## Compilación

Desde el directorio `shell/` y dentro de Linux:

```bash
make clean
make
```

Para ejecutar el shell:

```bash
./sys_shell
```

Para entrar al editor desde el shell:

```text
e_open prueba.txt
```

O ejecutar el editor directamente:

```bash
./cat_editor prueba.txt
```

## Pruebas automáticas

La suite está escrita completamente en C y requiere un entorno Linux/POSIX porque prueba también la interfaz interactiva mediante `forkpty()`.

Ejecutar:

```bash
make test
```

La suite comprueba:

- restricción de I/O y ausencia de funciones prohibidas;
- comandos base, inserción, búsqueda, metadata y copiar/pegar;
- casos borde y errores de rango;
- editor libre, navegación y atajos `Alt+S/Q/E/G`;
- integración `sys_shell -> e_open -> cat_editor -> retorno al shell`.

## Entorno de ejecución

El proyecto está diseñado para Linux/POSIX. En Windows, Python no incluye `pty` ni `termios`, por lo que las pruebas anteriores basadas en Python no son portables a ese entorno. Por esa razón la suite oficial del proyecto quedó implementada en C/POSIX y debe ejecutarse desde Linux, WSL o una máquina virtual Linux.
