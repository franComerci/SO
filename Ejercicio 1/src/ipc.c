#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

/* Limpia nombres de IPC de forma idempotente (para janitor) */
void ipc_force_unlink_names(void) {
    shm_unlink(SHM_NAME);
    sem_unlink(SEM_MUTEX_NAME);
    sem_unlink(SEM_EMPTY_NAME);
    sem_unlink(SEM_FILLED_NAME);
}

/* Inicializa los recursos de IPC.
 * Usa O_CREAT|O_EXCL para evitar colisiones con otra instancia viva.
 */
int ipc_init(ipc_t *ipc, long total_records) {
    memset(ipc, 0, sizeof(*ipc));
    ipc->shm_fd = -1;

    /* Crear SHM de forma exclusiva */
    ipc->shm_fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (ipc->shm_fd == -1) {
        if (errno == EEXIST) {
            fprintf(stderr, "[IPC] Otra instancia ya está usando %s. Abortando.\n", SHM_NAME);
        } else {
            perror("shm_open");
        }
        return -1;
    }

#ifdef __linux__
    if (ftruncate(ipc->shm_fd, sizeof(shm_data_t)) == -1) {
        perror("ftruncate");
        close(ipc->shm_fd);
        shm_unlink(SHM_NAME);
        return -1;
    }
#else
    /* En macOS puede fallar ftruncate; intentamos si hace falta y toleramos fallo. */
    off_t current_size = lseek(ipc->shm_fd, 0, SEEK_END);
    if (current_size < (off_t)sizeof(shm_data_t)) {
        if (ftruncate(ipc->shm_fd, sizeof(shm_data_t)) == -1) {
            perror("ftruncate (macOS, tolerado)");
            /* seguimos, el mmap de tamaño pedido funcionará igual */
        }
    }
#endif

    ipc->data = mmap(NULL, sizeof(shm_data_t), PROT_READ | PROT_WRITE, MAP_SHARED, ipc->shm_fd, 0);
    if (ipc->data == MAP_FAILED) {
        perror("mmap");
        close(ipc->shm_fd);
        shm_unlink(SHM_NAME);
        return -1;
    }

    /* Semáforos POSIX por nombre */
    ipc->mutex = sem_open(SEM_MUTEX_NAME, O_CREAT | O_EXCL, 0666, 1);
    if (ipc->mutex == SEM_FAILED) {
        perror("sem_open mutex");
        munmap(ipc->data, sizeof(shm_data_t));
        close(ipc->shm_fd);
        shm_unlink(SHM_NAME);
        return -1;
    }

    ipc->empty_slots = sem_open(SEM_EMPTY_NAME, O_CREAT | O_EXCL, 0666, BUFFER_SIZE);
    if (ipc->empty_slots == SEM_FAILED) {
        perror("sem_open empty_slots");
        sem_unlink(SEM_MUTEX_NAME);
        sem_close(ipc->mutex);
        munmap(ipc->data, sizeof(shm_data_t));
        close(ipc->shm_fd);
        shm_unlink(SHM_NAME);
        return -1;
    }

    ipc->filled_slots = sem_open(SEM_FILLED_NAME, O_CREAT | O_EXCL, 0666, 0);
    if (ipc->filled_slots == SEM_FAILED) {
        perror("sem_open filled_slots");
        sem_unlink(SEM_EMPTY_NAME);
        sem_close(ipc->empty_slots);
        sem_unlink(SEM_MUTEX_NAME);
        sem_close(ipc->mutex);
        munmap(ipc->data, sizeof(shm_data_t));
        close(ipc->shm_fd);
        shm_unlink(SHM_NAME);
        return -1;
    }

    /* Inicializar SHM */
    memset(ipc->data, 0, sizeof(shm_data_t));
    ipc->data->active = 1;
    ipc->data->next_id = 1;             /* IDs empiezan en 1 y se asignan al LEER */
    ipc->data->total_records = total_records;
    ipc->data->written_records = 0;
    ipc->data->head = 0;
    ipc->data->tail = 0;

    return 0;
}

/* Libera recursos IPC (idempotente) */
void ipc_cleanup(ipc_t *ipc) {
    if (ipc->data && ipc->data != MAP_FAILED) {
        munmap(ipc->data, sizeof(shm_data_t));
        ipc->data = NULL;
    }

    if (ipc->shm_fd != -1) {
        close(ipc->shm_fd);
        ipc->shm_fd = -1;
    }

    /* Unlinks por nombre: idempotentes */
    shm_unlink(SHM_NAME);

    if (ipc->mutex && ipc->mutex != SEM_FAILED) sem_close(ipc->mutex);
    if (ipc->empty_slots && ipc->empty_slots != SEM_FAILED) sem_close(ipc->empty_slots);
    if (ipc->filled_slots && ipc->filled_slots != SEM_FAILED) sem_close(ipc->filled_slots);

    sem_unlink(SEM_MUTEX_NAME);
    sem_unlink(SEM_EMPTY_NAME);
    sem_unlink(SEM_FILLED_NAME);

    printf("[IPC] Recursos limpiados.\n");
}

/* Productor: escribe un item con id=0 (el coordinador asigna el ID al consumir) */
int ipc_write(ipc_t *ipc, const record_t *record) {
    struct timespec ts;

    for (;;) {
        if (!ipc->data->active) return -1;

        if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
            perror("clock_gettime");
            return -1;
        }
        ts.tv_sec += 1;

#ifdef __APPLE__
        int ret;
        while ((ret = sem_trywait(ipc->empty_slots)) == -1 && errno == EAGAIN) {
            if (!ipc->data->active) return -1;
            struct timespec req = (struct timespec){0, 1000000}; /* 1ms */
            nanosleep(&req, NULL);
        }
#else
        int ret = sem_timedwait(ipc->empty_slots, &ts);
#endif
        if (ret == 0) break;

#ifndef __APPLE__
        if (errno == EINTR || errno == ETIMEDOUT) continue;
        perror("sem_timedwait(empty_slots)");
        return -1;
#endif
    }

    sem_wait(ipc->mutex);

    if (ipc->data->active) {
        record_t tmp = *record;
        tmp.id = 0; /* el ID lo setea el coordinador al leer */

        ipc->data->records[ipc->data->head] = tmp;
        ipc->data->head = (ipc->data->head + 1) % BUFFER_SIZE;

        sem_post(ipc->mutex);
        sem_post(ipc->filled_slots);
        return 0;
    } else {
        sem_post(ipc->mutex);
        sem_post(ipc->empty_slots);
        return -1;
    }
}

/* Consumidor (coordinador): lee un item, ASIGNA ID consecutivo y lo devuelve */
int ipc_read(ipc_t *ipc, record_t *record) {
    if (!ipc->data->active && ipc->data->written_records >= ipc->data->total_records)
        return -1;

    sem_wait(ipc->filled_slots);
    sem_wait(ipc->mutex);

    /* Extraer del buffer */
    *record = ipc->data->records[ipc->data->tail];
    ipc->data->tail = (ipc->data->tail + 1) % BUFFER_SIZE;

    /* ASIGNAR ID CONSECUTIVO AQUÍ (único punto del sistema) */
    record->id = ipc->data->next_id;
    ipc->data->next_id++;

    ipc->data->written_records++;

    sem_post(ipc->mutex);
    sem_post(ipc->empty_slots);

    return 0;
}

/* API opcional: no usada en esta solución, se mantiene por compatibilidad */
long ipc_get_next_id_block(ipc_t *ipc, int block_size) {
    long id_start;
    sem_wait(ipc->mutex);

    if (ipc->data->next_id > ipc->data->total_records) {
        id_start = -1;
    } else {
        id_start = ipc->data->next_id;
        ipc->data->next_id += block_size;
    }

    sem_post(ipc->mutex);
    return id_start;
}
