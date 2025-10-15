/* Garantiza prototipos POSIX (getopt, sigaction) visibles para LSP/compilador */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <signal.h>  // Ensure this is included for struct sigaction
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <getopt.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "ipc.h"
#include <string.h>

/* ---------- Config & estado ---------- */
/* Fallback: algunos entornos no exportan useconds_t sin feature macros */
#ifndef TP1_HAVE_USECONDS_T
#ifndef useconds_t
#define useconds_t unsigned int
#endif
#define TP1_HAVE_USECONDS_T 1
#endif

static useconds_t slow_us = 0;
static int janitor_pipe_fd = -1;
static pid_t janitor_pid = -1;

static ipc_t ipc;
static int is_coordinator = 0;
static volatile sig_atomic_t abort_due_child = 0;

/* ---------- Prototipos ---------- */
static void cleanup_handler(int signum);
static void sigchld_handler(int signum);
static void generator_process(int id, int total_records);
static void coordinator_process(int total_records);
static pid_t spawn_janitor(int read_fd, int write_fd);
static void show_help(const char *name);

/* ---------- Helpers ---------- */
static void show_help(const char *name) {
    printf("Uso: %s -g <generadores> -n <registros> [-s <microsegundos por registro>]\n", name);
    printf("  -g: Número de procesos generadores.\n");
    printf("  -n: Número total de registros a generar.\n");
    printf("  -s: (opcional) modo lento, microsegundos tras cada registro.\n");
}

/* ---------- Janitor ---------- */
static pid_t spawn_janitor(int read_fd, int write_fd) {
    pid_t pid = fork();
    if (pid == 0) {
        /* Importante: cerrar el write-end heredado para que EOF llegue al read-end */
        if (write_fd != -1) close(write_fd);
        char buf;
        while (read(read_fd, &buf, 1) > 0) {
            /* espera EOF; si el padre muere, read() termina por EOF o error */
        }
        ipc_force_unlink_names();
        _exit(0);
    }
    return pid;
}

/* ---------- Señales ---------- */
static void cleanup_handler(int signum) {
    (void)signum;
    fprintf(stderr, "\n[MAIN] Señal %d recibida. Limpiando recursos...\n", signum);

    if (ipc.data)
        ipc.data->active = 0;

    if (janitor_pipe_fd != -1) {
        close(janitor_pipe_fd); /* EOF → janitor limpia y termina */
        janitor_pipe_fd = -1;
    }

    if (is_coordinator) {
        if (ipc.filled_slots) sem_post(ipc.filled_slots);
        if (ipc.empty_slots)  sem_post(ipc.empty_slots);
        usleep(200000);
        ipc_cleanup(&ipc);
    }

    _exit(0);
}

/* Detecta muerte de un hijo */
static void sigchld_handler(int signum) {
    (void)signum;
    if (!is_coordinator || ipc.data == NULL) return;

    int status;
    pid_t pid;
    int should_abort = 0;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == janitor_pid) continue;

        if (WIFSIGNALED(status)) {
            fprintf(stderr, "[MAIN] Generador PID %d murió por señal %d. Abortando.\n",
                    pid, WTERMSIG(status));
            should_abort = 1;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            fprintf(stderr, "[MAIN] Generador PID %d terminó con error %d. Abortando.\n",
                    pid, WEXITSTATUS(status));
            should_abort = 1;
        }
    }

    if (should_abort && ipc.data->active) {
        abort_due_child = 1;
        ipc.data->active = 0;

        if (ipc.filled_slots) sem_post(ipc.filled_slots);

        if (janitor_pipe_fd != -1) {
            close(janitor_pipe_fd);
            janitor_pipe_fd = -1;
        }
    }
}

/* ---------- Generadores ---------- */
static void generator_process(int id, int total_records) {
    (void)total_records;
    printf("[GEN %d] Iniciado.\n", id);

    /* Restablecer handlers por defecto en los hijos */
    struct sigaction dfl; /* evito literal compuesto para compatibilidad de analizadores */
    memset(&dfl, 0, sizeof dfl);
    sigemptyset(&dfl.sa_mask);
    dfl.sa_flags = 0;
    dfl.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &dfl, NULL);
    sigaction(SIGINT,  &dfl, NULL);
    sigaction(SIGTERM, &dfl, NULL);

#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1) _exit(1);
#endif

    srand((unsigned)(time(NULL) ^ getpid()));

    for (;;) {
        if (!ipc.data || !ipc.data->active) break;

        record_t r;
        r.id = 0; /* el ID se asigna en el coordinador */
        int quantity = rand() % 101;
        snprintf(r.data, sizeof(r.data), "%d,%d", quantity, id);

        if (ipc_write(&ipc, &r) == -1)
            break;

        if (slow_us > 0)
            usleep(slow_us);
    }

    printf("[GEN %d] Termine.\n", id);
    _exit(0);
}

/* ---------- Coordinador (asigna ID y escribe CSV) ---------- */
static void coordinator_process(int total_records) {
    is_coordinator = 1;
    printf("[COORD] Iniciado. Esperando %d registros.\n", total_records);

    FILE *file = fopen("salida.csv", "w");
    if (!file) {
        perror("fopen salida.csv");
        if (ipc.data) ipc.data->active = 0;
        return;
    }
    fprintf(file, "ID,SKU,Cantidad,Generator\n");

    while (ipc.data && ipc.data->active && !abort_due_child) {
        record_t rec;
        if (ipc_read(&ipc, &rec) == 0) {
            /* rec.id fue ASIGNADO AQUÍ por ipc_read, 1..K sin huecos */
            fprintf(file, "%ld,SKU%06ld,%s\n", rec.id, rec.id, rec.data);

            if (ipc.data->written_records >= ipc.data->total_records) {
                /* objetivo alcanzado */
                break;
            }
        } else if (!ipc.data->active || abort_due_child) {
            break;
        }
    }

    fflush(file);
    fclose(file);

    if (!abort_due_child)
        printf("[COORD] Termine. Total escritos: %ld\n", ipc.data->written_records);
    else
        printf("[COORD] Abortado por caída de un generador. Escritos: %ld/%ld\n",
               ipc.data->written_records, ipc.data->total_records);

    if (ipc.data) ipc.data->active = 0;
}

/* ---------- main ---------- */
int main(int argc, char *argv[]) {
    int opt;
    long tmp;
    int num_generators = -1;
    int total_records  = -1;

    while ((opt = getopt(argc, argv, "g:n:s:")) != -1) {
        switch (opt) {
            case 'g': tmp = strtol(optarg, NULL, 10); num_generators = (int)tmp; break;
            case 'n': tmp = strtol(optarg, NULL, 10); total_records  = (int)tmp; break;
            case 's': tmp = strtol(optarg, NULL, 10); slow_us        = (useconds_t)tmp; break;
            default:  show_help(argv[0]); return 1;
        }
    }

    if (num_generators <= 0 || total_records <= 0) {
        show_help(argv[0]);
        return 1;
    }

    /* Señales del coordinador */
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = cleanup_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction sachld;
    sigemptyset(&sachld.sa_mask);
    /* SA_NOCLDSTOP puede no estar definido en ciertos entornos */
#ifdef SA_NOCLDSTOP
    sachld.sa_flags = SA_NOCLDSTOP;
#else
    sachld.sa_flags = 0;
#endif
    sachld.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sachld, NULL);

    /* Inicializar IPC (exclusivo: falla si ya hay otra instancia viva) */
    if (ipc_init(&ipc, total_records) != 0) {
        fprintf(stderr, "[MAIN] Error inicializando IPC.\n");
        return 1;
    }

    printf("[MAIN] Generadores: %d, Total: %d\n", num_generators, total_records);

    /* Crear janitor */
    int jfds[2] = {-1, -1};
    if (pipe(jfds) == 0) {
        janitor_pid = spawn_janitor(jfds[0], jfds[1]);
        close(jfds[0]); /* cerrar extremo de lectura en el padre */
        if (janitor_pid > 0) {
            janitor_pipe_fd = jfds[1];   /* el padre mantiene el write-end */
        } else {
            close(jfds[1]);
            janitor_pid = -1;
        }
    }

    /* Lanzar generadores */
    pid_t *pids = calloc((size_t)num_generators, sizeof(pid_t));
    for (int i = 0; i < num_generators; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            /* Hijo: cerrar write-end heredado del janitor */
            if (janitor_pipe_fd != -1) { close(janitor_pipe_fd); janitor_pipe_fd = -1; }
#ifdef __linux__
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            if (getppid() == 1) _exit(1);
#endif
            /* Hijo no reconfigura IPC: hereda handles ya abiertos */
            generator_process(i + 1, total_records);
            _exit(0);
        } else if (pid > 0) {
            pids[i] = pid;
        } else {
            perror("fork");
            ipc.data->active = 0;
            break;
        }
    }

    /* Coordinador */
    coordinator_process(total_records);

    if (abort_due_child) {
        printf("[MAIN] Abortando por caída de generador. Limpiando recursos...\n");

        for (int i = 0; i < num_generators; ++i)
            if (pids[i] > 0) kill(pids[i], SIGKILL);

        if (janitor_pid > 0)
            kill(janitor_pid, SIGKILL);

        while (waitpid(-1, NULL, WNOHANG) > 0) {}

        ipc_cleanup(&ipc);
        free(pids);
        return 1;
    }

    printf("[MAIN] Esperando que los generadores terminen...\n");
    signal(SIGCHLD, SIG_DFL);
    for (int i = 0; i < num_generators; ++i)
        if (pids[i] > 0) waitpid(pids[i], NULL, 0);

    printf("[MAIN] Todos los procesos han terminado. Limpiando...\n");

    /* Cerrar pipe → janitor detecta EOF y limpia si hiciera falta */
    if (janitor_pipe_fd != -1) { close(janitor_pipe_fd); janitor_pipe_fd = -1; }
    usleep(100000); /* 0.1s para que el janitor termine */

    ipc_cleanup(&ipc);

    /* Esperar al janitor sin bloquear indefinidamente */
    if (janitor_pid > 0) {
        int status;
        pid_t r = waitpid(janitor_pid, &status, WNOHANG);
        if (r == 0) {
            kill(janitor_pid, SIGTERM);
            usleep(50000);
            waitpid(janitor_pid, NULL, WNOHANG);
        }
    }

    free(pids);
    printf("[MAIN] Finalizado correctamente.\n");
    return 0;
}
