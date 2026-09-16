# Decisión de diseño: categoría `editor`

## Problema

El shell organiza sus comandos por categorías educativas. `datos` contiene operaciones atómicas sobre archivos: el usuario ejecuta una orden, se realiza una operación y el prompt regresa. `cat_editor` tiene un comportamiento diferente: después de `e_open <archivo>`, comienza una sesión interactiva propia y permanece activa hasta que el usuario decide salir.

## Alternativas consideradas

### Opción 1: incluirlo en `datos`

Era razonable considerarlo porque `cat_editor` utiliza `open`, `read`, `write`, `lseek`, `ftruncate` y `close`, igual que varias operaciones de `cat_datos.c`.

Sin embargo, compartir system calls no significa compartir la misma responsabilidad ni el mismo flujo de interacción. `d_read` y `d_copy` son comandos puntuales; `cat_editor` es una aplicación interactiva con su propio REPL.

### Opción 2: crear `editor`

El equipo decidió crear una categoría nueva porque la clasificación debe reflejar la responsabilidad y la naturaleza de la interacción, no solamente las syscalls utilizadas internamente.

Además, `e_open` tiene el mismo patrón de lanzamiento que `p_exec`: el shell crea un proceso hijo con `fork()`, ejecuta el programa con `execv()` y el padre espera con `waitpid()`. El propósito, sin embargo, es distinto, por lo que se reutiliza el patrón de ejecución sin mezclar responsabilidades.

## Ventajas

1. **Separación de responsabilidades:** `sys_shell` despacha; `cat_editor` administra la sesión de edición.
2. **Aislamiento por procesos:** el editor tiene un espacio de memoria independiente del shell.
3. **Modularidad:** futuras funciones de edición pueden crecer dentro de `editor` sin sobrecargar `datos`.
4. **Coherencia educativa:** el shell conserva la distinción entre operaciones puntuales y aplicaciones interactivas.

## Flujo

```text
Usuario
  |
  v
sys_shell
  | e_open archivo.txt
  +---- fork() ----> hijo
  |                  |
  |                  +---- execv("./cat_editor", ...)
  |                                |
  |                                v
  |                         sesión interactiva
  |                         Alt+S / Alt+Q / Alt+E / Alt+G
  |                                |
  +<----------- waitpid() ---------+
  |
  v
regreso al prompt del shell
```

## I/O y editor libre

La persistencia del archivo se realiza exclusivamente mediante `open`, `read`, `write`, `lseek`, `ftruncate` y `close`; `fstat` se utiliza para la metadata. No se usan `fopen`, `fread`, `fwrite` ni `fclose` en `cat_editor.c`.

La interfaz del editor libre usa `scanf("%c", ...)` para obtener cada carácter del teclado mientras la terminal está en modo RAW. De esta manera, los `read()` del editor quedan reservados para el archivo de texto y no existe `read(STDIN_FILENO, ...)`.

Los atajos son deliberadamente `Alt+S`, `Alt+Q`, `Alt+E` y `Alt+G`, porque en el entorno de desarrollo del equipo las combinaciones con Ctrl no se recibían de forma consistente. Cada combinación Alt+letra se interpreta como la secuencia ANSI `ESC` seguida de la letra.
