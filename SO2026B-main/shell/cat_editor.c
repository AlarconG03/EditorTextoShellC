/* ============================================================================
 * cat_editor.c
 * ----------------------------------------------------------------------------
 * Editor de texto interactivo por CLI (Linux/POSIX) - SO2026B
 * VERSIÓN CON ESTRUCTURA DE DATOS: LISTA LIGADA DE LÍNEAS / PALABRAS
 *
 * Estructura de datos (según lo indicado en clase):
 *
 *   Documento = lista doblemente enlazada de "Line" (líneas).
 *   Cada "Line" contiene, a su vez, otra lista enlazada de "Word" (palabras),
 *   donde cada espacio ' ' también se guarda como un nodo (palabra de
 *   longitud 1). Guardar los espacios como nodos permite reconstruir la
 *   línea EXACTAMENTE tal cual estaba, con las mismas separaciones, con solo
 *   concatenar los nodos en orden.
 *
 *        documento (Line*)
 *          |
 *          v
 *        [Línea 0] <-> [Línea 1] <-> [Línea 2] <-> ... <-> NULL
 *          |
 *          v
 *        [Palabra:"Hola"] -> [" "] -> [Palabra:"Mundo"] -> NULL
 *
 * Los comandos clasicos (o, p, a, d, i, s, m, y, x, q) operan sobre esta
 * estructura EN MEMORIA. El comando 'e' entra ademas a un modo de edicion
 * de pantalla completa; al guardar se reconstruye la misma lista ligada. El archivo en disco solo se toca en dos momentos:
 *
 *   1) Al abrir (o): se lee el archivo completo con syscalls de bajo nivel
 *      y se parsea (tokeniza) hacia la lista enlazada.
 *   2) Después de cada comando clásico que modifica el documento (a, d, i, x)
 *      y al guardar desde el editor libre, se reconstruye el contenido completo
 *      recorriendo la lista y se reescribe el archivo completo en disco.
 *
 * RESTRICCIÓN CRÍTICA DE I/O (se sigue cumpliendo igual que en la versión
 * anterior): toda lectura/escritura del archivo en disco usa EXCLUSIVAMENTE
 * open(), read(), write(), lseek(), ftruncate() y close(). NUNCA se usan
 * fopen/fread/fwrite/fclose. printf/scanf/fgets solo se usan para leer
 * comandos de STDIN e imprimir mensajes propios de la consola del editor.
 * ============================================================================
 */

#define _XOPEN_SOURCE 700  /* expone ftruncate(), fstat(), etc. en -std=c11 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <termios.h>

#define MAX_INPUT 4096

/* ============================================================================
 * INTERFAZ INTERACTIVA EN CLI (modo "pantalla completa" tipo vim/less)
 * ----------------------------------------------------------------------------
 * Usamos el buffer de pantalla alterno de la terminal (el mismo mecanismo
 * ANSI que usan vim, less, htop, nano) para que la visualizacion de la
 * estructura de datos se vea como una "pantalla" propia, separada de la
 * consola de comandos del editor. Se sale de esa pantalla capturando la
 * combinacion Alt+E en modo RAW (sin canonicalizar, sin eco), leyendo
 * caracter por caracter. Para cumplir estrictamente la restriccion de I/O del
 * enunciado, la captura del teclado se realiza mediante scanf("%c") (I/O
 * estandar), no mediante read() sobre STDIN_FILENO.
 * En esta version los atajos de navegacion usan ALT porque en el entorno
 * utilizado para la practica las combinaciones CTRL no se reciben de forma
 * consistente. El terminal normalmente envia ALT+letra como ESC seguido de
 * la letra, por ejemplo ALT+E -> ESC e.
 * ============================================================================
 */
#define KEY_ALT_G   2001  /* Alt+G: ayuda */
#define KEY_ALT_Q   2002  /* Alt+Q: guardar/salir o regresar */
#define KEY_ALT_S   2003  /* Alt+S: guardar */
#define KEY_ALT_E   2004  /* Alt+E: estructura/regresar */
#define KEY_ESC     0x1B

#define ED_STATUS_MAX 256

static void report_syscall_error(const char *operation);

typedef struct {
    char **lines;
    size_t count;
    size_t capacity;
    size_t row;
    size_t col;
    size_t top_row;
    size_t left_col;
    int dirty;
} EditorBuffer;

/* Colores ANSI para la visualizacion (igual espiritu al que se ve en clase) */
#define COL_RESET   "\033[0m"
#define COL_TITLE   "\033[1;97m"   /* blanco brillante - titulo */
#define COL_ADDR    "\033[1;32m"   /* verde - direcciones de memoria */
#define COL_LINEHDR "\033[0;90m"   /* gris - "Linea N -> len:" */
#define COL_LABEL   "\033[0;36m"   /* cian - "-> Palabra:" */
#define COL_TEXT    "\033[1;34m"   /* azul - contenido de la palabra */
#define COL_FOOTER  "\033[0;33m"   /* amarillo - pie de pantalla */
#define COL_STATUS  "\033[0;36m"   /* cian - barra de estado */

static struct termios g_orig_termios;
static int g_raw_mode_active = 0;

/* Guarda la configuracion original de la terminal y activa modo RAW:
 * desactiva el eco (ECHO), el modo canonico linea-a-linea (ICANON) y el
 * procesamiento de señales por teclado (ISIG), para poder leer Alt+E
 * como un byte crudo en vez de que la terminal lo interprete. */
static void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) {
        report_syscall_error("tcgetattr");
        return;
    }
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        report_syscall_error("tcsetattr");
        return;
    }
    g_raw_mode_active = 1;
}

/* Restaura la terminal a como estaba antes de enable_raw_mode(). */
static void disable_raw_mode(void) {
    if (!g_raw_mode_active) return;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios) == -1) {
        report_syscall_error("tcsetattr (restaurar)");
    }
    g_raw_mode_active = 0;
}

/* Entra al buffer de pantalla alterno (secuencia ANSI 1049h, la misma que
 * usa vim al abrir): la terminal guarda lo que habia en pantalla y muestra
 * un lienzo limpio. Al salir (1049l) se restaura exactamente lo que habia
 * antes, como si nunca hubiera pasado nada -- asi la sesion del shell de
 * comandos del editor queda intacta debajo. */
static void enter_alt_screen(void) {
    printf("\033[?1049h\033[H\033[2J");
    fflush(stdout);
}

static void leave_alt_screen(void) {
    printf("\033[?1049l");
    fflush(stdout);
}

/* ============================================================================
 * ESTRUCTURA DE DATOS: LISTA LIGADA ANIDADA (Líneas -> Palabras)
 * ============================================================================
 */

/* Nodo de la lista INTERNA: una palabra (o un espacio) dentro de una línea */
typedef struct Word {
    char *text;          /* palabra, reservada dinámicamente (heap/malloc) */
    int   len;            /* longitud de esa palabra en bytes               */
    struct Word *next;    /* siguiente palabra de la MISMA línea            */
} Word;

/* Nodo de la lista EXTERNA: una línea del documento */
typedef struct Line {
    Word *words;          /* cabeza de la lista anidada de palabras */
    int   len;             /* longitud total de la línea (suma de palabras) */
    struct Line *prev;
    struct Line *next;    /* siguiente línea del documento */
} Line;

/* Documento completo en memoria: lista doblemente enlazada de líneas */
static Line *g_head = NULL;
static Line *g_tail = NULL;
static int   g_line_count = 0;

/* Portapapeles: una línea "suelta" (no enlazada al documento), con su
 * propia lista anidada de palabras, copiada independientemente. */
typedef struct {
    Word *words;
    int   len;
    int   valid;
} Clipboard;
static Clipboard g_clip = { NULL, 0, 0 };

/* Estado del archivo en disco */
static int  g_fd = -1;
static char g_filename[512] = {0};

/* ---------------------------------------------------------------------- */
/* Utilidades de cadenas                                                  */
/* ---------------------------------------------------------------------- */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' ||
                        s[len-1] == ' '  || s[len-1] == '\t')) {
        s[len-1] = '\0';
        len--;
    }
    return s;
}

/* ============================================================================
 * OPERACIONES SOBRE LA LISTA ANIDADA DE PALABRAS (nivel interno de una línea)
 * ============================================================================
 */

/* Crea un nodo Word reservando memoria dinámica para su texto (heap). */
static Word *word_create(const char *text, int len) {
    if (len < 0) return NULL;
    Word *w = malloc(sizeof(Word));
    if (!w) {
        printf("Error: no se pudo reservar memoria para Word.\n");
        return NULL;
    }
    w->text = malloc((size_t)len + 1);
    if (!w->text) {
        printf("Error: no se pudo reservar memoria para el texto de Word.\n");
        free(w);
        return NULL;
    }
    memcpy(w->text, text, (size_t)len);
    w->text[len] = '\0';
    w->len = len;
    w->next = NULL;
    return w;
}

/* Libera toda la lista de palabras de una línea. */
static void word_list_free(Word *w) {
    while (w) {
        Word *next = w->next;
        free(w->text);
        free(w);
        w = next;
    }
}

/* Clona (copia profunda) una lista de palabras -> usado por y/x (portapapeles)
 * para que copiar/pegar no comparta memoria con el documento original. */
static Word *word_list_clone(Word *src, int *out_total_len) {
    Word *head = NULL, *tail = NULL;
    int total = 0;
    for (Word *w = src; w != NULL; w = w->next) {
        Word *copy = word_create(w->text, w->len);
        if (!copy) {
            word_list_free(head);
            if (out_total_len) *out_total_len = 0;
            return NULL;
        }
        if (!head) head = copy; else tail->next = copy;
        tail = copy;
        total += w->len;
    }
    if (out_total_len) *out_total_len = total;
    return head;
}

/* TOKENIZA una cadena cruda (una línea de texto) en su lista anidada de
 * palabras. Los espacios se guardan como nodos individuales de longitud 1,
 * para poder reconstruir la línea exactamente igual concatenando en orden. */
static Word *tokenize_line(const char *raw, int raw_len, int *out_total_len) {
    Word *head = NULL, *tail = NULL;
    int total = 0;
    int i = 0;
    /* Si la linea viene de un archivo con finales de linea estilo Windows
     * (CRLF), el '\r' queda pegado antes del '\n' que ya se usó como
     * separador de línea. Lo recortamos aquí para no arrastrarlo como si
     * fuera parte de la última palabra. */
    if (raw_len > 0 && raw[raw_len - 1] == '\r') raw_len--;
    while (i < raw_len) {
        Word *w;
        if (raw[i] == ' ') {
            w = word_create(" ", 1);
            i++;
        } else {
            int start = i;
            while (i < raw_len && raw[i] != ' ') i++;
            w = word_create(raw + start, i - start);
        }
        if (!w) {
            word_list_free(head);
            if (out_total_len) *out_total_len = 0;
            return NULL;
        }
        if (!head) head = w; else tail->next = w;
        tail = w;
        total += w->len;
    }
    if (out_total_len) *out_total_len = total;
    return head;
}

/* Reconstruye el texto completo de una línea concatenando sus palabras.
 * Devuelve un buffer reservado con malloc (el llamador debe liberarlo). */
static char *line_to_string(Line *l) {
    if (!l || l->len < 0) return NULL;
    char *buf = malloc((size_t)l->len + 1);
    if (!buf) {
        printf("Error: no se pudo reservar memoria para reconstruir la linea.\n");
        return NULL;
    }
    int pos = 0;
    for (Word *w = l->words; w != NULL; w = w->next) {
        memcpy(buf + pos, w->text, (size_t)w->len);
        pos += w->len;
    }
    buf[pos] = '\0';
    return buf;
}

/* ============================================================================
 * OPERACIONES SOBRE LA LISTA DE LÍNEAS (nivel documento)
 * ============================================================================
 */

static Line *line_create_from_text(const char *text) {
    if (!text) text = "";
    Line *l = malloc(sizeof(Line));
    if (!l) {
        printf("Error: no se pudo reservar memoria para Line.\n");
        return NULL;
    }
    l->words = tokenize_line(text, (int)strlen(text), &l->len);
    if (strlen(text) > 0 && !l->words) {
        free(l);
        return NULL;
    }
    l->prev = l->next = NULL;
    return l;
}

static void line_free(Line *l) {
    word_list_free(l->words);
    free(l);
}

static void doc_free_all(void) {
    Line *l = g_head;
    while (l) {
        Line *next = l->next;
        line_free(l);
        l = next;
    }
    g_head = g_tail = NULL;
    g_line_count = 0;
}

static void doc_append(Line *l) {
    l->prev = g_tail;
    l->next = NULL;
    if (g_tail) g_tail->next = l; else g_head = l;
    g_tail = l;
    g_line_count++;
}

/* Ubica el nodo Line en la posición n (1-indexada) recorriendo la lista. */
static Line *doc_get(int n) {
    if (n < 1 || n > g_line_count) return NULL;
    Line *l = g_head;
    for (int i = 1; i < n; i++) l = l->next;
    return l;
}

/* Inserta una línea nueva ANTES de la posición n (1-indexada).
 * Si n == g_line_count+1, inserta al final. */
static int doc_insert_at(int n, Line *newline) {
    if (n == g_line_count + 1 || g_line_count == 0) {
        doc_append(newline);
        return 1;
    }
    Line *target = doc_get(n);
    if (!target) return 0;

    newline->prev = target->prev;
    newline->next = target;
    if (target->prev) target->prev->next = newline; else g_head = newline;
    target->prev = newline;
    g_line_count++;
    return 1;
}

static int doc_remove_at(int n) {
    Line *l = doc_get(n);
    if (!l) return 0;

    if (l->prev) l->prev->next = l->next; else g_head = l->next;
    if (l->next) l->next->prev = l->prev; else g_tail = l->prev;

    line_free(l);
    g_line_count--;
    return 1;
}

static void report_syscall_error(const char *operation) {
    perror(operation);
}

/* ============================================================================
 * PERSISTENCIA A DISCO (únicamente open/read/write/lseek/ftruncate/close)
 * ============================================================================
 */

static int ensure_open(void) {
    if (g_fd < 0) {
        printf("Error: no hay ningun archivo abierto. Use 'o [archivo]' primero.\n");
        return 0;
    }
    return 1;
}

/* Escribe todos los bytes solicitados. write() puede escribir menos bytes
 * de los pedidos, por lo que repetimos hasta completar el buffer. */
static int write_all_fd(int fd, const char *buf, size_t count) {
    size_t written = 0;
    while (written < count) {
        ssize_t n = write(fd, buf + written, count - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            report_syscall_error("write");
            return 0;
        }
        if (n == 0) {
            printf("Error: write() no progreso en el archivo.\n");
            return 0;
        }
        written += (size_t)n;
    }
    return 1;
}

/* Lee TODO el contenido crudo del archivo hacia un buffer dinámico. */
static char *read_whole_file(size_t *out_size) {
    if (!ensure_open()) return NULL;

    off_t filesz = lseek(g_fd, 0, SEEK_END);
    if (filesz == (off_t)-1) {
        report_syscall_error("lseek(END)");
        return NULL;
    }
    if (lseek(g_fd, 0, SEEK_SET) == (off_t)-1) {
        report_syscall_error("lseek(SET)");
        return NULL;
    }

    if (filesz < 0 || (uintmax_t)filesz > (uintmax_t)SIZE_MAX - 1) {
        printf("Error: tamano de archivo no valido para un buffer en memoria.\n");
        return NULL;
    }

    size_t capacity = (size_t)filesz + 1;
    char *buf = malloc(capacity > 0 ? capacity : 1);
    if (!buf) {
        printf("Error: no se pudo reservar memoria para leer el archivo.\n");
        return NULL;
    }

    size_t total = 0;
    while (total < (size_t)filesz) {
        ssize_t r = read(g_fd, buf + total, (size_t)filesz - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            report_syscall_error("read");
            free(buf);
            return NULL;
        }
        if (r == 0) {
            printf("Error: fin de archivo inesperado durante la lectura.\n");
            free(buf);
            return NULL;
        }
        total += (size_t)r;
    }

    buf[total] = '\0';
    if (out_size) *out_size = total;
    return buf;
}

/* Parsea el contenido crudo del archivo hacia la lista ligada de líneas. */
static int load_document_from_disk(void) {
    doc_free_all();

    size_t size = 0;
    char *raw = read_whole_file(&size);
    if (!raw) return 0;

    if (size == 0) {
        free(raw);
        return 1;
    }

    size_t i = 0, start = 0;
    while (i < size) {
        if (raw[i] == '\n') {
            Line *l = malloc(sizeof(Line));
            if (!l) {
                printf("Error: no se pudo reservar memoria para Line.\n");
                free(raw);
                doc_free_all();
                return 0;
            }
            l->words = tokenize_line(raw + start, (int)(i - start), &l->len);
            if ((i - start) > 0 && !l->words) {
                free(l);
                free(raw);
                doc_free_all();
                return 0;
            }
            l->prev = l->next = NULL;
            doc_append(l);
            start = i + 1;
        }
        i++;
    }

    /* Si el archivo no termina en '\\n', la ultima linea "colgante" tambien
     * se agrega para proteger contra archivos externos. */
    if (start < size) {
        Line *l = malloc(sizeof(Line));
        if (!l) {
            printf("Error: no se pudo reservar memoria para Line.\n");
            free(raw);
            doc_free_all();
            return 0;
        }
        l->words = tokenize_line(raw + start, (int)(size - start), &l->len);
        if (!l->words) {
            free(l);
            free(raw);
            doc_free_all();
            return 0;
        }
        l->prev = l->next = NULL;
        doc_append(l);
    }

    free(raw);
    return 1;
}

/* Reconstruye TODO el documento desde la lista ligada y reescribe el
 * archivo completo en disco, usando solo syscalls de bajo nivel. */
static int save_document_to_disk(void) {
    if (g_fd < 0) return 0;

    size_t total_size = 0;
    for (Line *l = g_head; l != NULL; l = l->next) {
        if ((size_t)l->len > SIZE_MAX - total_size - 1) {
            printf("Error: el documento es demasiado grande para guardar en memoria.\n");
            return 0;
        }
        total_size += (size_t)l->len + 1;
    }

    char *buf = malloc(total_size > 0 ? total_size : 1);
    if (!buf) {
        report_syscall_error("malloc (buffer de escritura)");
        return 0;
    }

    size_t pos = 0;
    for (Line *l = g_head; l != NULL; l = l->next) {
        char *text = line_to_string(l);
        if (!text) {
            printf("Error: no se pudo reconstruir una linea para guardar.\n");
            free(buf);
            return 0;
        }
        memcpy(buf + pos, text, (size_t)l->len);
        pos += (size_t)l->len;
        buf[pos++] = '\n';
        free(text);
    }

    if (lseek(g_fd, 0, SEEK_SET) == (off_t)-1) {
        report_syscall_error("lseek(SET) al guardar");
        free(buf);
        return 0;
    }
    if (ftruncate(g_fd, 0) == -1) {
        report_syscall_error("ftruncate");
        free(buf);
        return 0;
    }
    if (total_size > 0 && !write_all_fd(g_fd, buf, total_size)) {
        free(buf);
        return 0;
    }

    free(buf);
    return 1;
}

/* ============================================================================
 * COMANDOS
 * ============================================================================
 */

static void cmd_open(const char *filename) {
    if (filename == NULL || strlen(filename) == 0) {
        printf("Uso: o [archivo]\n");
        return;
    }
    if (g_fd >= 0) {
        if (close(g_fd) == -1) report_syscall_error("close");
        g_fd = -1;
    }
    int fd = open(filename, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        report_syscall_error("open");
        return;
    }
    g_fd = fd;
    strncpy(g_filename, filename, sizeof(g_filename) - 1);
    g_filename[sizeof(g_filename) - 1] = '\0';

    if (!load_document_from_disk()) {
        if (close(g_fd) == -1) report_syscall_error("close");
        g_fd = -1;
        g_filename[0] = '\0';
        printf("No se pudo cargar el archivo en memoria.\n");
        return;
    }
    printf("Archivo '%s' abierto (fd=%d). %d linea(s) cargada(s) en memoria.\n",
           g_filename, g_fd, g_line_count);
}

static void cmd_print(const char *arg) {
    if (!ensure_open()) return;

    if (arg == NULL || strlen(arg) == 0) {
        for (Line *l = g_head; l != NULL; l = l->next) {
            char *text = line_to_string(l);
            if (!text) {
                printf("Error: no se pudo reconstruir una linea para imprimir.\n");
                return;
            }
            printf("%s\n", text);
            free(text);
        }
        return;
    }

    int n = atoi(arg);
    Line *l = doc_get(n);
    if (!l) {
        printf("Error: la linea %d no existe.\n", n);
        return;
    }
    char *text = line_to_string(l);
    if (!text) {
        printf("Error: no se pudo reconstruir la linea para imprimir.\n");
        return;
    }
    printf("%s\n", text);
    free(text);
}

static void cmd_append(const char *text) {
    if (!ensure_open()) return;
    if (text == NULL) text = "";

    Line *l = line_create_from_text(text);
    if (!l) {
        printf("Error: no se pudo reservar memoria para la nueva linea.\n");
        return;
    }
    doc_append(l);
    if (!save_document_to_disk()) {
        printf("Error: la linea quedo en memoria, pero no pudo guardarse en disco.\n");
        return;
    }
    printf("Linea agregada al final (linea %d).\n", g_line_count);
}

static void cmd_delete(const char *arg) {
    if (!ensure_open()) return;
    if (arg == NULL || strlen(arg) == 0) {
        printf("Uso: d [n]\n");
        return;
    }
    int n = atoi(arg);
    if (!doc_remove_at(n)) {
        printf("Error: la linea %d no existe.\n", n);
        return;
    }
    if (!save_document_to_disk()) {
        printf("Error: la eliminacion quedo en memoria, pero no pudo guardarse en disco.\n");
        return;
    }
    printf("Linea %d eliminada.\n", n);
}

static void cmd_insert(const char *arg) {
    if (!ensure_open()) return;
    if (arg == NULL || strlen(arg) == 0) {
        printf("Uso: i [n] [texto]\n");
        return;
    }

    char argcopy[MAX_INPUT];
    strncpy(argcopy, arg, sizeof(argcopy) - 1);
    argcopy[sizeof(argcopy)-1] = '\0';

    char *sep = strchr(argcopy, ' ');
    if (!sep) {
        printf("Uso: i [n] [texto]\n");
        return;
    }
    *sep = '\0';
    int n = atoi(argcopy);
    char *text = trim(sep + 1);

    Line *l = line_create_from_text(text);
    if (!l) {
        printf("Error: no se pudo reservar memoria para la linea.\n");
        return;
    }
    if (!doc_insert_at(n, l)) {
        line_free(l);
        printf("Error: no se puede insertar en la linea %d (fuera de rango).\n", n);
        return;
    }
    if (!save_document_to_disk()) {
        printf("Error: la insercion quedo en memoria, pero no pudo guardarse en disco.\n");
        return;
    }
    printf("Texto insertado en la linea %d.\n", n);
}

static void cmd_search(const char *word) {
    if (!ensure_open()) return;
    if (word == NULL || strlen(word) == 0) {
        printf("Uso: s [palabra]\n");
        return;
    }

    int n = 1;
    int found = 0;
    for (Line *l = g_head; l != NULL; l = l->next, n++) {
        char *text = line_to_string(l);
        if (!text) {
            printf("Error: no se pudo reconstruir una linea para buscar.\n");
            return;
        }
        if (strstr(text, word) != NULL) {
            printf("%d: %s\n", n, text);
            found = 1;
        }
        free(text);
    }
    if (!found) printf("No se encontraron coincidencias para '%s'.\n", word);
}

static void cmd_metadata(void) {
    if (!ensure_open()) return;

    struct stat st;
    if (fstat(g_fd, &st) != 0) {
        report_syscall_error("fstat");
        return;
    }

    char perms[11];
    perms[0] = S_ISDIR(st.st_mode) ? 'd' : '-';
    perms[1] = (st.st_mode & S_IRUSR) ? 'r' : '-';
    perms[2] = (st.st_mode & S_IWUSR) ? 'w' : '-';
    perms[3] = (st.st_mode & S_IXUSR) ? 'x' : '-';
    perms[4] = (st.st_mode & S_IRGRP) ? 'r' : '-';
    perms[5] = (st.st_mode & S_IWGRP) ? 'w' : '-';
    perms[6] = (st.st_mode & S_IXGRP) ? 'x' : '-';
    perms[7] = (st.st_mode & S_IROTH) ? 'r' : '-';
    perms[8] = (st.st_mode & S_IWOTH) ? 'w' : '-';
    perms[9] = (st.st_mode & S_IXOTH) ? 'x' : '-';
    perms[10] = '\0';

    char timebuf[64];
    struct tm *tm_info = localtime(&st.st_mtime);
    if (!tm_info) {
        printf("Error: no se pudo convertir la fecha de modificacion.\n");
        return;
    }
    if (strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info) == 0) {
        printf("Error: no se pudo formatear la fecha de modificacion.\n");
        return;
    }

    printf("---- Metadata de '%s' ----\n", g_filename);
    printf("Tamano       : %ld bytes\n", (long)st.st_size);
    printf("Permisos     : %s (%o)\n", perms, st.st_mode & 0777);
    printf("Inodo        : %lu\n", (unsigned long)st.st_ino);
    printf("Modificado   : %s\n", timebuf);
    printf("Lineas en RAM: %d\n", g_line_count);
}

static void cmd_yank(const char *arg) {
    if (!ensure_open()) return;
    if (arg == NULL || strlen(arg) == 0) {
        printf("Uso: y [n]\n");
        return;
    }
    int n = atoi(arg);
    Line *l = doc_get(n);
    if (!l) {
        printf("Error: la linea %d no existe.\n", n);
        return;
    }

    word_list_free(g_clip.words);
    g_clip.words = word_list_clone(l->words, &g_clip.len);
    if (l->words != NULL && g_clip.words == NULL) {
        g_clip.len = 0;
        g_clip.valid = 0;
        printf("Error: no se pudo copiar la linea por falta de memoria.\n");
        return;
    }
    g_clip.valid = 1;

    printf("Linea %d copiada al portapapeles.\n", n);
}

static void cmd_paste(const char *arg) {
    if (!ensure_open()) return;
    if (!g_clip.valid) {
        printf("Error: el portapapeles esta vacio. Use 'y [n]' primero.\n");
        return;
    }

    int n = (arg == NULL || strlen(arg) == 0) ? g_line_count + 1 : atoi(arg);

    Line *l = malloc(sizeof(Line));
    if (!l) {
        printf("Error: no se pudo reservar memoria para pegar la linea.\n");
        return;
    }
    l->words = word_list_clone(g_clip.words, &l->len);
    if (g_clip.words != NULL && !l->words) {
        free(l);
        printf("Error: no se pudo pegar por falta de memoria.\n");
        return;
    }
    l->prev = l->next = NULL;

    if (!doc_insert_at(n, l)) {
        line_free(l);
        printf("Error: no se puede pegar en la linea %d (fuera de rango).\n", n);
        return;
    }
    if (!save_document_to_disk()) {
        printf("Error: el pegado quedo en memoria, pero no pudo guardarse en disco.\n");
        return;
    }
    printf("Portapapeles pegado en la linea %d.\n", n);
}

/* Muestra la estructura de datos interna (lista de líneas -> lista de
 * palabras), incluyendo direcciones de memoria, tal como se pide para
 * demostrar visualmente el uso de listas ligadas anidadas.
 *
 * Esta es la interfaz interactiva en CLI: entra a un buffer de pantalla
 * alterno (igual que vim/less), pinta la estructura con colores ANSI, y
 * se queda esperando -en modo RAW, byte a byte- a que el usuario presione
 * Alt+E para volver exactamente a donde estaba en el editor. La captura de
 * cada byte se hace con scanf("%c"), manteniendo read()/write() exclusivamente
 * para el archivo de texto. Mientras esta en esta pantalla, ningun otro
 * caracter hace nada (a proposito: es una vista de solo lectura). */
static void get_terminal_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row >= 4 && ws.ws_col >= 20) {
        *rows = (int)ws.ws_row;
        *cols = (int)ws.ws_col;
        return;
    }
    *rows = 24;
    *cols = 80;
}

static int editor_buffer_reserve(EditorBuffer *ed, size_t needed) {
    if (needed <= ed->capacity) return 1;
    size_t cap = ed->capacity ? ed->capacity : 8;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2) return 0;
        cap *= 2;
    }
    if (cap > SIZE_MAX / sizeof(*ed->lines)) return 0;
    char **tmp = realloc(ed->lines, cap * sizeof(*tmp));
    if (!tmp) return 0;
    ed->lines = tmp;
    ed->capacity = cap;
    return 1;
}

static int editor_buffer_push_line(EditorBuffer *ed, const char *text) {
    if (!editor_buffer_reserve(ed, ed->count + 1)) return 0;
    size_t n = strlen(text);
    ed->lines[ed->count] = malloc(n + 1);
    if (!ed->lines[ed->count]) return 0;
    memcpy(ed->lines[ed->count], text, n + 1);
    ed->count++;
    return 1;
}

static void editor_buffer_free(EditorBuffer *ed) {
    for (size_t i = 0; i < ed->count; i++) free(ed->lines[i]);
    free(ed->lines);
    memset(ed, 0, sizeof(*ed));
}

static int editor_buffer_from_document(EditorBuffer *ed) {
    memset(ed, 0, sizeof(*ed));
    for (Line *l = g_head; l; l = l->next) {
        char *text = line_to_string(l);
        int ok = editor_buffer_push_line(ed, text);
        free(text);
        if (!ok) {
            editor_buffer_free(ed);
            return 0;
        }
    }
    if (ed->count == 0 && !editor_buffer_push_line(ed, "")) {
        editor_buffer_free(ed);
        return 0;
    }
    ed->row = 0;
    ed->col = 0;
    ed->top_row = 0;
    ed->left_col = 0;
    ed->dirty = 0;
    return 1;
}

static int editor_insert_bytes(char **linep, size_t pos, const char *src, size_t n) {
    char *line = *linep;
    size_t len = strlen(line);
    if (pos > len || n > SIZE_MAX - len - 1) return 0;
    char *tmp = realloc(line, len + n + 1);
    if (!tmp) return 0;
    memmove(tmp + pos + n, tmp + pos, len - pos + 1);
    memcpy(tmp + pos, src, n);
    *linep = tmp;
    return 1;
}

static int editor_delete_range(char **linep, size_t pos, size_t n) {
    char *line = *linep;
    size_t len = strlen(line);
    if (pos > len) return 0;
    if (n > len - pos) n = len - pos;
    memmove(line + pos, line + pos + n, len - pos - n + 1);
    char *tmp = realloc(line, (len - n) + 1);
    if (tmp) *linep = tmp;
    return 1;
}

static int editor_split_line(EditorBuffer *ed) {
    char *line = ed->lines[ed->row];
    size_t len = strlen(line);
    char *right = malloc(len - ed->col + 1);
    if (!right) return 0;
    memcpy(right, line + ed->col, len - ed->col + 1);
    line[ed->col] = '\0';

    if (!editor_buffer_reserve(ed, ed->count + 1)) {
        free(right);
        return 0;
    }
    memmove(&ed->lines[ed->row + 2], &ed->lines[ed->row + 1],
            (ed->count - ed->row - 1) * sizeof(*ed->lines));
    ed->lines[ed->row + 1] = right;
    ed->count++;
    ed->row++;
    ed->col = 0;
    ed->dirty = 1;
    return 1;
}

static int editor_join_with_previous(EditorBuffer *ed) {
    if (ed->row == 0) return 0;
    size_t prev_len = strlen(ed->lines[ed->row - 1]);
    char *prev = ed->lines[ed->row - 1];
    char *cur = ed->lines[ed->row];
    size_t cur_len = strlen(cur);
    char *tmp = realloc(prev, prev_len + cur_len + 1);
    if (!tmp) return 0;
    memcpy(tmp + prev_len, cur, cur_len + 1);
    ed->lines[ed->row - 1] = tmp;
    free(cur);
    memmove(&ed->lines[ed->row], &ed->lines[ed->row + 1],
            (ed->count - ed->row - 1) * sizeof(*ed->lines));
    ed->count--;
    ed->row--;
    ed->col = prev_len;
    ed->dirty = 1;
    return 1;
}

static int editor_join_with_next(EditorBuffer *ed) {
    if (ed->row + 1 >= ed->count) return 0;
    size_t len = strlen(ed->lines[ed->row]);
    size_t next_len = strlen(ed->lines[ed->row + 1]);
    char *tmp = realloc(ed->lines[ed->row], len + next_len + 1);
    if (!tmp) return 0;
    memcpy(tmp + len, ed->lines[ed->row + 1], next_len + 1);
    ed->lines[ed->row] = tmp;
    free(ed->lines[ed->row + 1]);
    memmove(&ed->lines[ed->row + 1], &ed->lines[ed->row + 2],
            (ed->count - ed->row - 2) * sizeof(*ed->lines));
    ed->count--;
    ed->dirty = 1;
    return 1;
}

static int editor_sync_to_document(EditorBuffer *ed) {
    doc_free_all();
    for (size_t i = 0; i < ed->count; i++) {
        Line *l = line_create_from_text(ed->lines[i]);
        if (!l) {
            doc_free_all();
            return 0;
        }
        doc_append(l);
    }
    return 1;
}

static int editor_save(EditorBuffer *ed) {
    if (!ensure_open()) return 0;
    if (!editor_sync_to_document(ed)) return 0;
    if (!save_document_to_disk()) return 0;
    ed->dirty = 0;
    return 1;
}

static int read_key(void);

static void editor_help_screen(void) {
    enter_alt_screen();
    enable_raw_mode();

    int rows, cols;
    get_terminal_size(&rows, &cols);
    printf("\033[H\033[2J");
    printf(COL_TITLE " CAT_EDITOR - AYUDA " COL_RESET "\n\n");
    const char *items[] = {
        "Alt+S   Guardar archivo",
        "Alt+Q   Guardar y salir del editor libre",
        "Alt+E   Ver estructura de lista ligada",
        "Alt+G   Mostrar esta ayuda",
        "Flechas  Mover el cursor",
        "Home/End Ir al inicio/final de linea",
        "Enter    Partir la linea",
        "Backspace Borrar atras / unir con la linea anterior",
        "Delete   Borrar adelante / unir con la siguiente linea",
        "Escribir Insertar texto en la posicion del cursor"
    };
    size_t n = sizeof(items) / sizeof(items[0]);
    for (size_t i = 0; i < n && (int)i + 3 < rows - 1; i++)
        printf("  %.*s\n", cols > 4 ? cols - 4 : 1, items[i]);
    printf("\n" COL_FOOTER "Alt+G: volver" COL_RESET "\n");
    fflush(stdout);

    if (g_raw_mode_active) {
        int key;
        do {
            key = read_key();
        } while (key != -1 && key != KEY_ALT_G && key != KEY_ALT_E && key != KEY_ALT_Q && key != KEY_ESC);
    }
    disable_raw_mode();
    leave_alt_screen();
}

static int read_key(void) {
    unsigned char c;
    if (scanf("%c", (char *)&c) != 1) return -1;
    if (c != KEY_ESC) return c;

    /* Las flechas/Home/End/Delete usan secuencias ANSI que empiezan por ESC.
     * Los atajos ALT+letra llegan como ESC + letra. Se leen con scanf("%c")
     * para que STDIN use exclusivamente I/O estandar de C. */
    unsigned char seq[3];
    if (scanf("%c", (char *)&seq[0]) != 1) return KEY_ESC;

    switch (seq[0]) {
        case 's': case 'S': return KEY_ALT_S;
        case 'q': case 'Q': return KEY_ALT_Q;
        case 'e': case 'E': return KEY_ALT_E;
        case 'g': case 'G': return KEY_ALT_G;
        default: break;
    }

    if (seq[0] == '[' || seq[0] == 'O') {
        if (scanf("%c", (char *)&seq[1]) != 1) return KEY_ESC;
        if (seq[1] >= '0' && seq[1] <= '9') {
            if (scanf("%c", (char *)&seq[2]) == 1 && seq[2] == '~') {
                if (seq[1] == '1' || seq[1] == '7') return 1001; /* Home */
                if (seq[1] == '4' || seq[1] == '8') return 1002; /* End */
                if (seq[1] == '3') return 1003;                  /* Delete */
            }
        } else {
            switch (seq[1]) {
                case 'A': return 1004; /* Up */
                case 'B': return 1005; /* Down */
                case 'C': return 1006; /* Right */
                case 'D': return 1007; /* Left */
                case 'H': return 1001; /* Home */
                case 'F': return 1002; /* End */
                default: break;
            }
        }
    }
    return KEY_ESC;
}

static void editor_clamp_cursor(EditorBuffer *ed) {
    if (ed->count == 0) {
        editor_buffer_push_line(ed, "");
    }
    if (ed->row >= ed->count) ed->row = ed->count - 1;
    size_t len = strlen(ed->lines[ed->row]);
    if (ed->col > len) ed->col = len;
}

static void editor_render(EditorBuffer *ed, const char *status) {
    int rows, cols;
    get_terminal_size(&rows, &cols);
    const int header_rows = 2;
    const int footer_rows = 2;
    int visible_rows = rows - header_rows - footer_rows;
    if (visible_rows < 1) visible_rows = 1;
    const int gutter = 7; /* " 123456 " */
    int content_cols = cols - gutter;
    if (content_cols < 1) content_cols = 1;

    editor_clamp_cursor(ed);
    if (ed->row < ed->top_row) ed->top_row = ed->row;
    if (ed->row >= ed->top_row + (size_t)visible_rows)
        ed->top_row = ed->row - (size_t)visible_rows + 1;
    if (ed->col < ed->left_col) ed->left_col = ed->col;
    if (ed->col >= ed->left_col + (size_t)content_cols)
        ed->left_col = ed->col - (size_t)content_cols + 1;

    printf("\033[H\033[2J");
    char title[ED_STATUS_MAX];
    const char *fname = g_filename[0] ? g_filename : "(sin archivo)";
    snprintf(title, sizeof(title), " CAT_EDITOR  |  %.*s%s",
             (int)sizeof(title) - 24, fname, ed->dirty ? " *" : "");
    printf(COL_TITLE "%-*.*s" COL_RESET "\n", cols, cols, title);
    printf(COL_LINEHDR " %-6s  %-*s" COL_RESET "\n", "LINEA", content_cols, "Texto");

    for (int vr = 0; vr < visible_rows; vr++) {
        size_t idx = ed->top_row + (size_t)vr;
        if (idx < ed->count) {
            const char *line = ed->lines[idx];
            size_t len = strlen(line);
            size_t start = ed->left_col < len ? ed->left_col : len;
            size_t avail = (size_t)content_cols;
            size_t shown = len - start;
            if (shown > avail) shown = avail;
            printf(" %5zu  %.*s", idx + 1, (int)shown, line + start);
        } else {
            printf(" %5s  ~", "");
        }
        printf("\033[K\n");
    }

    char footer1[ED_STATUS_MAX];
    snprintf(footer1, sizeof(footer1), " %-*.*s", cols - 1, cols - 1,
             status ? status : "");
    printf(COL_STATUS "%s" COL_RESET "\n", footer1);
    printf(COL_FOOTER " Alt+S Guardar   Alt+Q Guardar/Salir   Alt+E Estructura   Alt+G Ayuda " COL_RESET);
    fflush(stdout);

    size_t screen_row = (ed->row - ed->top_row) + header_rows + 1;
    size_t screen_col = gutter + (ed->col - ed->left_col) + 1;
    if (screen_row >= 1 && screen_row <= (size_t)rows && screen_col >= 1 && screen_col <= (size_t)cols) {
        printf("\033[%zu;%zuH\033[?25h", screen_row, screen_col);
        fflush(stdout);
    }
}

static void cmd_visualize(void);

static int cmd_edit_screen(void) {
    if (!ensure_open()) return 0;

    EditorBuffer ed;
    if (!editor_buffer_from_document(&ed)) {
        printf("Error: no se pudo reservar memoria para el editor.\n");
        return 0;
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        printf("Error: el editor libre requiere una terminal interactiva.\n");
        editor_buffer_free(&ed);
        return 0;
    }

    enter_alt_screen();
    enable_raw_mode();
    if (!g_raw_mode_active) {
        leave_alt_screen();
        editor_buffer_free(&ed);
        printf("Error: no se pudo activar el modo RAW de la terminal.\n");
        return 0;
    }

    int running = 1;
    char status[ED_STATUS_MAX] = "Editor libre listo";

    while (running) {
        editor_render(&ed, status);
        int key = read_key();
        if (key == -1) {
            snprintf(status, sizeof(status), "Entrada cerrada: guardando y saliendo...");
            editor_save(&ed);
            break;
        }

        switch (key) {
            case KEY_ALT_S:
                if (editor_save(&ed)) snprintf(status, sizeof(status), "Guardado correctamente");
                else snprintf(status, sizeof(status), "Error al guardar");
                break;
            case KEY_ALT_Q:
                if (editor_save(&ed)) {
                    snprintf(status, sizeof(status), "Guardado. Regresando a comandos...");
                    running = 0;
                } else {
                    snprintf(status, sizeof(status), "No se pudo guardar; Alt+Q no cerrara sin guardar");
                }
                break;
            case KEY_ALT_E:
                disable_raw_mode();
                leave_alt_screen();
                cmd_visualize();
                enter_alt_screen();
                enable_raw_mode();
                if (!g_raw_mode_active) running = 0;
                break;
            case KEY_ALT_G:
                disable_raw_mode();
                leave_alt_screen();
                editor_help_screen();
                enter_alt_screen();
                enable_raw_mode();
                if (!g_raw_mode_active) running = 0;
                break;
            case 1004:
                if (ed.row > 0) ed.row--;
                editor_clamp_cursor(&ed);
                break;
            case 1005:
                if (ed.row + 1 < ed.count) ed.row++;
                editor_clamp_cursor(&ed);
                break;
            case 1006:
                if (ed.col < strlen(ed.lines[ed.row])) ed.col++;
                break;
            case 1007:
                if (ed.col > 0) ed.col--;
                break;
            case 1001:
                ed.col = 0;
                break;
            case 1002:
                ed.col = strlen(ed.lines[ed.row]);
                break;
            case 1003:
                if (ed.col < strlen(ed.lines[ed.row])) {
                    editor_delete_range(&ed.lines[ed.row], ed.col, 1);
                    ed.dirty = 1;
                } else if (editor_join_with_next(&ed)) {
                    /* joined next line */
                }
                break;
            case '\r':
            case '\n':
                if (!editor_split_line(&ed)) snprintf(status, sizeof(status), "Error de memoria al partir la linea");
                break;
            case 127:
            case 8:
                if (ed.col > 0) {
                    ed.col--;
                    editor_delete_range(&ed.lines[ed.row], ed.col, 1);
                    ed.dirty = 1;
                } else if (editor_join_with_previous(&ed)) {
                    /* joined previous line */
                }
                break;
            case KEY_ESC:
                snprintf(status, sizeof(status), "Esc no cierra: usa Alt+Q para guardar y salir");
                break;
            default:
                if (key >= 32 && key <= 126) {
                    char ch = (char)key;
                    if (editor_insert_bytes(&ed.lines[ed.row], ed.col, &ch, 1)) {
                        ed.col++;
                        ed.dirty = 1;
                    } else {
                        snprintf(status, sizeof(status), "Sin memoria para insertar");
                    }
                }
                break;
        }
    }

    disable_raw_mode();
    leave_alt_screen();
    editor_buffer_free(&ed);
    return 1;
}

/* Muestra la estructura de datos interna (lista de líneas -> palabras). */
static void cmd_visualize(void) {
    if (!ensure_open()) return;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        printf("Error: la visualizacion interactiva requiere una terminal.\n");
        return;
    }

    enter_alt_screen();
    enable_raw_mode();
    if (!g_raw_mode_active) {
        leave_alt_screen();
        printf("Error: no se pudo activar el modo RAW de la terminal.\n");
        return;
    }

    while (1) {
        int rows, cols;
        get_terminal_size(&rows, &cols);
        printf("\033[H\033[2J");
        printf(COL_TITLE "==== ESTRUCTURA DE DATOS (LISTA LIGADA DE LINEAS/PALABRAS) ====\n" COL_RESET);
        printf(COL_LINEHDR " Archivo: %-.*s\033[K\n\n" COL_RESET,
               cols > 12 ? cols - 12 : 1, g_filename);

        int used = 3;
        int max_lines = rows - 5;
        int idx = 0;
        for (Line *l = g_head; l != NULL && used < max_lines; l = l->next, idx++) {
            char hdr[ED_STATUS_MAX];
            snprintf(hdr, sizeof(hdr), "[%p] Linea %d -> len: %d", (void *)l, idx, l->len);
            printf(COL_ADDR "%.*s" COL_RESET "\033[K\n", cols, hdr);
            used++;
            for (Word *w = l->words; w != NULL && used < max_lines; w = w->next) {
                char rowbuf[ED_STATUS_MAX];
                snprintf(rowbuf, sizeof(rowbuf), "   -> Palabra: '%s' (len: %d)", w->text, w->len);
                printf(COL_LABEL "%.*s" COL_RESET "\033[K\n", cols, rowbuf);
                used++;
            }
        }
        if (used < max_lines) {
            printf("\033[K\n");
            used++;
            printf(COL_FOOTER " %-*s" COL_RESET "\n", cols - 1, "Alt+E Regresar   Alt+Q Regresar al editor   Alt+G Ayuda");
        } else {
            printf(COL_FOOTER " %-*s" COL_RESET "\n", cols - 1, "Vista recortada   Alt+E Regresar   Alt+Q Regresar al editor   Alt+G Ayuda");
        }
        fflush(stdout);

        int key = read_key();
        if (key == -1 || key == KEY_ALT_E || key == KEY_ESC) break;
        if (key == KEY_ALT_Q) break;
        if (key == KEY_ALT_G) {
            disable_raw_mode();
            leave_alt_screen();
            editor_help_screen();
            enter_alt_screen();
            enable_raw_mode();
            if (!g_raw_mode_active) break;
        }
    }

    disable_raw_mode();
    leave_alt_screen();
}

static void cmd_quit(void) {
    if (g_fd >= 0) {
        if (close(g_fd) == -1) report_syscall_error("close");
        g_fd = -1;
    }
    doc_free_all();
    word_list_free(g_clip.words);
    g_clip.words = NULL;
    g_clip.len = 0;
    g_clip.valid = 0;
    printf("Saliendo de cat_editor. Hasta luego.\n");
    exit(0);
}

/* ---------------------------------------------------------------------- */
/* Bucle principal / parser de comandos                                   */
/* ---------------------------------------------------------------------- */
static void print_help(void) {
    printf("Comandos disponibles:\n");
    printf("  o [archivo]      - abrir/crear archivo (carga la lista ligada en memoria)\n");
    printf("  p [n]            - imprimir linea n (o todo el archivo sin n)\n");
    printf("  a [texto]        - agregar linea al final\n");
    printf("  d [n]            - borrar linea n\n");
    printf("  i [n] [texto]    - insertar texto en la linea n\n");
    printf("  s [palabra]      - buscar palabra en el archivo\n");
    printf("  m                - metadata del archivo (fstat)\n");
    printf("  y [n]            - copiar linea n al portapapeles\n");
    printf("  x [n]            - pegar portapapeles en la linea n (o al final)\n");
    printf("  v                - visualizar la estructura de datos (Alt+E para volver)\n");
    printf("  e                - abrir editor libre de pantalla completa (Alt+Q para salir)\n");
    printf("  q                - salir\n");
}

int run_editor(int argc, char *argv[]) {
    /* Desactivamos el buffer de stdin para que scanf("%c") entregue cada
     * tecla del modo RAW inmediatamente, sin usar read() sobre STDIN. */
    if (setvbuf(stdin, NULL, _IONBF, 0) != 0) {
        printf("Advertencia: no se pudo dejar STDIN sin buffer.\n");
    }

    /* Salvaguarda: si el programa termina de cualquier forma (exit normal,
     * o incluso una salida inesperada) mientras la terminal quedo en modo
     * RAW por culpa de 'v', esto la devuelve a su estado normal para que
     * el shell padre (y la sesion de terminal del usuario) no queden con
     * el eco de teclado apagado. */
    atexit(disable_raw_mode);

    if (argc > 1) {
        cmd_open(argv[1]);
    }

    char line[MAX_INPUT];
    printf("cat_editor - 'e' editor libre, 'v' estructura, 'h' ayuda, 'q' salir.\n");

    while (1) {
        printf("cat_editor:%s> ", g_filename[0] ? g_filename : "(sin archivo)");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            cmd_quit();
            break;
        }

        char *trimmed = trim(line);
        if (strlen(trimmed) == 0) continue;

        char cmd = trimmed[0];
        char *rest = trimmed + 1;
        while (*rest == ' ' || *rest == '\t') rest++;
        rest = trim(rest);
        char *arg = (strlen(rest) > 0) ? rest : NULL;

        switch (cmd) {
            case 'o': cmd_open(arg); break;
            case 'p': cmd_print(arg); break;
            case 'a': cmd_append(arg); break;
            case 'd': cmd_delete(arg); break;
            case 'i': cmd_insert(arg); break;
            case 's': cmd_search(arg); break;
            case 'm': cmd_metadata(); break;
            case 'y': cmd_yank(arg); break;
            case 'x': cmd_paste(arg); break;
            case 'v': cmd_visualize(); break;
            case 'e': cmd_edit_screen(); break;
            case 'q': cmd_quit(); break;
            case 'h': print_help(); break;
            default:
                printf("Comando desconocido: '%c'. Escriba 'h' para ayuda.\n", cmd);
        }
    }
    return 0;
}

#ifndef CAT_EDITOR_NO_MAIN
int main(int argc, char *argv[]) {
    return run_editor(argc, argv);
}
#endif