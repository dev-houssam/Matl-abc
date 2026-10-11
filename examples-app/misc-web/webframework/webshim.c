/* webshim.c — pont C pour le framework web MATL-ABC.
 *
 * 1) THREADS : matlabc_thread_spawn(fn, arg) lance un pthread DETACHE
 *    qui appelle fn(arg) — le serveur y passe un funcptr MATL-ABC
 *    (caste en rawptr) et le descripteur du client accepte.
 *
 * 2) SQLITE3 : petit wrapper au-dessus de sqlite3_get_table (l'API
 *    tableau-de-resultats de SQLite, parfaite pour un pont simple :
 *    pas de callbacks, pas de parametres de sortie multiples).
 *    Les signatures sqlite3 sont declarees a la main (void* opaques)
 *    -- pas besoin de sqlite3.h, on lie directement libsqlite3.so.0.
 *
 * Compilation :
 *   gcc -shared -fPIC webshim.c -o libwebshim.so \
 *       -l:libsqlite3.so.0 -lpthread
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 1. threads ---------- */

typedef void (*mabc_worker_fn)(int);

struct spawn_args { mabc_worker_fn fn; int arg; };

static void *thread_tramp(void *p) {
    struct spawn_args *sa = (struct spawn_args *)p;
    mabc_worker_fn fn = sa->fn;
    int arg = sa->arg;
    free(sa);
    fn(arg);
    return NULL;
}

/* Lance fn(arg) dans un thread detache. Retourne 0 si OK. */
int matlabc_thread_spawn(void *fn, int arg) {
    struct spawn_args *sa = malloc(sizeof *sa);
    if (!sa) return -1;
    sa->fn  = (mabc_worker_fn)fn;
    sa->arg = arg;
    pthread_t tid;
    int r = pthread_create(&tid, NULL, thread_tramp, sa);
    if (r != 0) { free(sa); return -1; }
    pthread_detach(tid);
    return 0;
}

/* ---------- 2. sqlite3 (declarations manuelles, void* opaques) ---------- */

extern int  sqlite3_open(const char *filename, void **db);
extern int  sqlite3_close(void *db);
extern int  sqlite3_exec(void *db, const char *sql,
                         int (*cb)(void*,int,char**,char**),
                         void *arg, char **errmsg);
extern int  sqlite3_get_table(void *db, const char *sql,
                              char ***result, int *nrow, int *ncol,
                              char **errmsg);
extern void sqlite3_free_table(char **result);
extern void sqlite3_free(void *p);

/* Ouvre (ou cree) la base. Retourne le handle, ou NULL si echec. */
void *matlabc_db_open(const char *path) {
    void *db = NULL;
    if (sqlite3_open(path, &db) != 0) return NULL;
    return db;
}

int matlabc_db_close(void *db) {
    return sqlite3_close(db);
}

/* Execute un SQL sans resultat (CREATE/INSERT/DELETE...). 0 si OK. */
int matlabc_db_exec(void *db, const char *sql) {
    char *err = NULL;
    int r = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return r;
}

/* ---- requetes SELECT via sqlite3_get_table ----
 * Le resultat est un tableau de chaines : la ligne 0 contient les
 * noms de colonnes, puis nrow lignes de ncol cellules.
 * On expose : query -> handle opaque ; rows/cols ; cell(r,c) ; free.
 */
struct db_result {
    char **table;
    int nrow;
    int ncol;
};

void *matlabc_db_query(void *db, const char *sql) {
    struct db_result *res = malloc(sizeof *res);
    if (!res) return NULL;
    char *err = NULL;
    int r = sqlite3_get_table(db, sql, &res->table, &res->nrow, &res->ncol, &err);
    if (err) sqlite3_free(err);
    if (r != 0) { free(res); return NULL; }
    return res;
}

int matlabc_db_result_rows(void *h) {
    return h ? ((struct db_result *)h)->nrow : 0;
}

int matlabc_db_result_cols(void *h) {
    return h ? ((struct db_result *)h)->ncol : 0;
}

/* Cellule (ligne r, colonne c) — r=0 est la PREMIERE ligne de
 * DONNEES (les noms de colonnes internes de get_table sont sautes). */
const char *matlabc_db_result_cell(void *h, int r, int c) {
    struct db_result *res = (struct db_result *)h;
    if (!res || r < 0 || r >= res->nrow || c < 0 || c >= res->ncol) return "";
    const char *cell = res->table[(r + 1) * res->ncol + c];
    return cell ? cell : "";
}

void matlabc_db_result_free(void *h) {
    struct db_result *res = (struct db_result *)h;
    if (!res) return;
    sqlite3_free_table(res->table);
    free(res);
}
