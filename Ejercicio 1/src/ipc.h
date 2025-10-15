#ifndef IPC_H
#define IPC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <semaphore.h>

/* Tamaño del buffer circular */
#define BUFFER_SIZE 64

/* Nombres base de IPC (se usan tal cual; como solo el padre hace ipc_init,
 * y luego se heredan los FDs/handles por fork(), no hace falta re-abrir por nombre en hijos).
 * Para evitar colisiones entre ejecuciones, ipc_init usa O_CREAT|O_EXCL; si existen, aborta.
 */
#define SHM_NAME        "/shm_tp1_robust"
#define SEM_MUTEX_NAME  "/sem_mutex_tp1_robust"
#define SEM_EMPTY_NAME  "/sem_empty_tp1_robust"
#define SEM_FILLED_NAME "/sem_filled_tp1_robust"

/* Registro producido por los generadores.
 * El ID ahora se completa en el COORDINADOR al leer (no en el productor).
 * Esto garantiza 1..K sin huecos ni duplicados incluso ante abortos.
 */
typedef struct {
    long id;            /* lo setea el coordinador al consumir */
    char data[256];     /* "Cantidad,Generator" */
} record_t;

/* Estructura en SHM compartida por todos los procesos */
typedef struct {
    volatile int active;      /* 1: seguir; 0: terminar */
    long next_id;             /* próximo ID para asignar en el coordinador */
    long total_records;       /* objetivo global (para info/log) */
    long written_records;     /* cantidad de registros ya escritos al CSV */
    int head;                 /* índice de escritura (productor) */
    int tail;                 /* índice de lectura (consumidor) */
    record_t records[BUFFER_SIZE]; /* los productores llenan data; id=0 aquí */
} shm_data_t;

/* Estructura local que guarda handles de IPC */
typedef struct {
    shm_data_t *data;         /* puntero mapeado a la SHM */
    sem_t *mutex;             /* exclusión mutua para data */
    sem_t *empty_slots;       /* lugares disponibles en buffer */
    sem_t *filled_slots;      /* elementos disponibles en buffer */
    int shm_fd;               /* descriptor de la SHM */
} ipc_t;

/* API */
int  ipc_init(ipc_t *ipc, long total_records);
void ipc_cleanup(ipc_t *ipc);
int  ipc_write(ipc_t *ipc, const record_t *record);  /* productor escribe (id=0) */
int  ipc_read(ipc_t *ipc, record_t *record);         /* coordinador lee y ASIGNA ID */

/* Unlink idempotente para janitor (por nombre) */
void ipc_force_unlink_names(void);

#endif /* IPC_H */
