#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
// Habilita prototipos POSIX (getopt, sigaction, strdup, fileno)
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <poll.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#define BUFFER_SIZE 2048

static volatile sig_atomic_t g_running = 1;
static int g_server_fd = -1;
static int g_sigpipe_fds[2] = { -1, -1 }; // self-pipe to wake main loop on SIGCHLD

volatile sig_atomic_t current_clients = 0;

void handle_client(int client_socket, const char *db_file, int lifeline_fd);
void process_command(int client_socket, const char *command, FILE *db, int *transaction_active);
void show_help(const char *name);
void sigchld_handler(int s);
void read_config(const char *config_file, char *address_buf, int buf_size, int *port);
void send_error(int client_socket, const char *msg);
void send_ok(int client_socket, const char *msg);
static char *trim(char *s);
static void sigterm_handler(int sig);
static void promote_waiting(int *waitlist, int *w_count, int wait_queue,
                            volatile sig_atomic_t *cur_clients, int max_clients,
                            int server_fd, const char *db_file);
static void send_wait_positions(int *waitlist, int w_count, int wait_queue);

// Portable file lock helpers using fcntl (works on POSIX systems)
static int file_lock(FILE *f, int exclusive, int nonblock) {
    if (!f) return -1;
    struct flock fl;
    fl.l_type = exclusive ? F_WRLCK : F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0; // lock the whole file
    return fcntl(fileno(f), nonblock ? F_SETLK : F_SETLKW, &fl);
}

static int file_unlock(FILE *f) {
    if (!f) return -1;
    struct flock fl;
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    return fcntl(fileno(f), F_SETLK, &fl);
}

int main(int argc, char *argv[]) {
    int opt;
    char address_buf[40] = "127.0.0.1";
    char *address = address_buf;
    int port = 8080;
    char *db_file = NULL;
    int max_clients = 5;
    int wait_queue = 10;

    // Leer configuración desde archivo primero
    read_config("server.conf", address_buf, sizeof(address_buf), &port);
    address = address_buf; // Apuntar a nuestro buffer

    while ((opt = getopt(argc, argv, "ha:p:f:c:w:")) != -1) {
        switch (opt) {
            case 'h':
                show_help(argv[0]);
                exit(EXIT_SUCCESS);
            case 'a':
                strncpy(address_buf, optarg, sizeof(address_buf) - 1);
                address_buf[sizeof(address_buf) - 1] = '\0';
                break;
            case 'p':
                port = atoi(optarg);
                break;
            case 'f':
                db_file = optarg;
                break;
            case 'c':
                max_clients = atoi(optarg);
                break;
            case 'w':
                wait_queue = atoi(optarg);
                break;
            default:
                show_help(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (db_file == NULL) {
        fprintf(stderr, "Error: Debe especificar la ruta al archivo CSV con -f.\n");
        show_help(argv[0]);
        exit(EXIT_FAILURE);
    }

    int server_fd, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    g_server_fd = server_fd;

    int optval = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(address);
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, wait_queue) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Servidor escuchando en %s:%d (DB=%s)\n", address, port, db_file);

    // Self-pipe for SIGCHLD notifications
    if (pipe(g_sigpipe_fds) == -1) {
        perror("pipe self");
        exit(EXIT_FAILURE);
    }
    // Make non-blocking
    int flags0 = fcntl(g_sigpipe_fds[0], F_GETFL, 0); if (flags0 != -1) fcntl(g_sigpipe_fds[0], F_SETFL, flags0 | O_NONBLOCK);
    int flags1 = fcntl(g_sigpipe_fds[1], F_GETFL, 0); if (flags1 != -1) fcntl(g_sigpipe_fds[1], F_SETFL, flags1 | O_NONBLOCK);

    struct sigaction sa;
    sa.sa_handler = sigchld_handler; // reap all dead processes
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    // Graceful shutdown on SIGINT/SIGTERM
    struct sigaction sat;
    sigemptyset(&sat.sa_mask);
    sat.sa_flags = 0;
    sat.sa_handler = sigterm_handler;
    if (sigaction(SIGINT, &sat, NULL) == -1) perror("sigaction SIGINT");
    if (sigaction(SIGTERM, &sat, NULL) == -1) perror("sigaction SIGTERM");

    // Waitlist storage (simple linear queue)
    int capacity = wait_queue > 0 ? wait_queue : 1;
    int *waitlist = malloc(sizeof(int) * capacity);
    if (!waitlist) { perror("malloc waitlist"); exit(EXIT_FAILURE); }
    int w_count = 0;

    // poll fds (rebuilt each loop including current waiters)
    struct pollfd fds[2 + 1024];

    while (g_running) {
        // Rebuild poll set
        fds[0].fd = server_fd; fds[0].events = POLLIN; fds[0].revents = 0;
        fds[1].fd = g_sigpipe_fds[0]; fds[1].events = POLLIN; fds[1].revents = 0;
        int nfds = 2;
        for (int i = 0; i < w_count; ++i) {
            fds[nfds].fd = waitlist[i];
            fds[nfds].events = POLLIN | POLLHUP | POLLERR;
            fds[nfds].revents = 0;
            nfds++;
        }

        int pr = poll(fds, nfds, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll main");
            break;
        }
        if (fds[1].revents & POLLIN) {
            // Drain self-pipe
            char tmp[64]; while (read(g_sigpipe_fds[0], tmp, sizeof(tmp)) > 0) {}
            // Try to promote waiting clients
            promote_waiting(waitlist, &w_count, wait_queue, &current_clients, max_clients, server_fd, db_file);
            send_wait_positions(waitlist, w_count, wait_queue);
        }
        if (fds[0].revents & POLLIN) {
            client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
            if (client_socket < 0) {
                if (!g_running) break;
                if (errno == EINTR) continue;
                perror("accept failed");
                continue;
            }

            // If slots available now, serve immediately
            if (current_clients < max_clients) {
                int lifeline[2];
                if (pipe(lifeline) == -1) { perror("pipe lifeline"); lifeline[0] = lifeline[1] = -1; }

                sigset_t block, prev;
                sigemptyset(&block); sigaddset(&block, SIGCHLD);
                if (sigprocmask(SIG_BLOCK, &block, &prev) == -1) perror("sigprocmask block");

                printf("Nueva conexión desde %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                pid_t pid = fork();
                if (pid < 0) {
                    perror("fork failed");
                    close(client_socket);
                    if (lifeline[0] != -1) close(lifeline[0]);
                    if (lifeline[1] != -1) close(lifeline[1]);
                } else if (pid == 0) {
                    close(server_fd);
                    if (lifeline[1] != -1) close(lifeline[1]);
#ifdef __linux__
                    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
                    if (sigprocmask(SIG_SETMASK, &prev, NULL) == -1) perror("sigprocmask restore child");
                    handle_client(client_socket, db_file, lifeline[0]);
                    exit(EXIT_SUCCESS);
                } else {
                    // Parent
                    if (lifeline[0] != -1) close(lifeline[0]);
                    current_clients++;
                }
                if (sigprocmask(SIG_SETMASK, &prev, NULL) == -1) perror("sigprocmask restore");
            } else {
                // Queue or reject
                if (w_count < wait_queue) {
                    int pos = w_count + 1;
                    char msg[128];
                    snprintf(msg, sizeof(msg), "INFO;WAIT;pos=%d cap=%d\n", pos, wait_queue);
                    (void)send(client_socket, msg, strlen(msg), 0);
                    waitlist[w_count++] = client_socket;
                    printf("[SERVER] Encolado cliente %s:%d (pos %d/%d)\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), pos, wait_queue);
                } else {
                    printf("[SERVER] Máximo de clientes (%d) y lista espera (%d) llenos. Rechazando %s:%d\n", max_clients, wait_queue, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                    send_error(client_socket, "Servidor ocupado. Lista de espera llena.");
                    close(client_socket);
                }
            }
        }

        // Handle waiters events: answer INFO;WAIT on input, remove on HUP/ERR
        for (int i = 0; i < w_count;) {
            int idx = 2 + i;
            if (!fds[idx].revents) { i++; continue; }
            int wsock = waitlist[i];
            if (fds[idx].revents & (POLLHUP | POLLERR)) {
                close(wsock);
                if (i < w_count - 1) memmove(&waitlist[i], &waitlist[i+1], sizeof(int) * (w_count - i - 1));
                w_count--;
                send_wait_positions(waitlist, w_count, wait_queue);
                continue; // do not increment i, we shifted
            }
            if (fds[idx].revents & POLLIN) {
                char tmpbuf[256];
                int n = recv(wsock, tmpbuf, sizeof(tmpbuf)-1, 0);
                if (n <= 0) {
                    close(wsock);
                    if (i < w_count - 1) memmove(&waitlist[i], &waitlist[i+1], sizeof(int) * (w_count - i - 1));
                    w_count--;
                    send_wait_positions(waitlist, w_count, wait_queue);
                    continue;
                }
                int pos = i + 1;
                char msg[128];
                snprintf(msg, sizeof(msg), "INFO;WAIT;pos=%d cap=%d\n", pos, wait_queue);
                (void)send(wsock, msg, strlen(msg), 0);
            }
            i++;
        }
    }

    printf("[SERVER] Cerrando servidor...\n");
    if (server_fd != -1) close(server_fd);
    if (g_sigpipe_fds[0] != -1) close(g_sigpipe_fds[0]);
    if (g_sigpipe_fds[1] != -1) close(g_sigpipe_fds[1]);
    // Notify and close any queued sockets (best-effort)
    for (int i = 0; i < w_count; ++i) {
        const char *endmsg = "ERROR;Servidor apagándose\n";
        (void)send(waitlist[i], endmsg, strlen(endmsg), 0);
        close(waitlist[i]);
    }
    free(waitlist);
    // Recolectar cualquier hijo pendiente
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    printf("[SERVER] Apagado completo.\n");
    return 0;
}

void handle_client(int client_socket, const char *db_file, int lifeline_fd) {
    char buffer[BUFFER_SIZE];
    int transaction_active = 0;
    FILE *db = NULL;

    // Ignore SIGPIPE so we can handle client disconnects gracefully
    signal(SIGPIPE, SIG_IGN);

    struct pollfd fds[2];
    fds[0].fd = client_socket; fds[0].events = POLLIN | POLLHUP | POLLERR; fds[0].revents = 0;
    fds[1].fd = lifeline_fd;   fds[1].events = POLLIN | POLLHUP | POLLERR; fds[1].revents = 0;

    for (;;) {
        int pr = poll(fds, (lifeline_fd != -1 ? 2 : 1), -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        // Si cae la lifeline (padre murió), salimos.
        if (lifeline_fd != -1 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            // Consumir para limpiar y salir.
            char tmp[1];
            (void)read(lifeline_fd, tmp, 1);
            printf("[SERVER/child] Padre finalizado. Cerrando cliente.\n");
            break;
        }

        if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;

        memset(buffer, 0, BUFFER_SIZE);
        int bytes_read = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_read <= 0) {
            if (transaction_active && db) {
                fflush(db);
                file_unlock(db);
                fclose(db);
                db = NULL;
                transaction_active = 0;
            }
            if (bytes_read == 0) {
                printf("[SERVER] Cliente cerró la conexión.\n");
            } else {
                printf("[SERVER] Error de recepción de cliente: %s\n", strerror(errno));
            }
            break;
        }

        buffer[strcspn(buffer, "\n")] = 0; // Remove newline

        if (strcmp(buffer, "BEGIN_TRANSACTION") == 0) {
            if (transaction_active) {
                send_error(client_socket, "Ya hay una transacción activa.");
            } else {
                db = fopen(db_file, "r+");
                if (db == NULL) {
                    perror("fopen");
                    send_error(client_socket, "No se pudo abrir la base de datos.");
                    continue;
                }
                if (file_lock(db, 1, 1) == -1) {
                    if (errno == EWOULDBLOCK) {
                        send_error(client_socket, "La base de datos está bloqueada por otra transacción. Reintente más tarde.");
                    } else {
                        perror("file_lock");
                        send_error(client_socket, "No se pudo bloquear la base de datos.");
                    }
                    fclose(db);
                    db = NULL;
                } else {
                    transaction_active = 1;
                    send_ok(client_socket, "Transacción iniciada.");
                }
            }
        } else if (strcmp(buffer, "COMMIT_TRANSACTION") == 0) {
            if (!transaction_active) {
                send_error(client_socket, "No hay una transacción activa.");
            } else {
                fflush(db);
                file_unlock(db);
                fclose(db);
                db = NULL;
                transaction_active = 0;
                send_ok(client_socket, "Transacción finalizada.");
            }
        } else if (strncmp(buffer, "QUERY;", 6) == 0 || 
                   strncmp(buffer, "INSERT;", 7) == 0 || 
                   strncmp(buffer, "DELETE;", 7) == 0 || 
                   strncmp(buffer, "UPDATE;", 7) == 0) {
            if (transaction_active) {
                process_command(client_socket, buffer, db, &transaction_active);
            } else {
                 FILE *temp_db = fopen(db_file, "r");
                if (temp_db == NULL) {
                    send_error(client_socket, "No se pudo abrir la base de datos para consulta.");
                } else {
                    if (file_lock(temp_db, 0, 1) == -1) {
                        if (errno == EWOULDBLOCK) {
                            send_error(client_socket, "La base de datos está bloqueada por una transacción activa. Reintente más tarde.");
                        } else {
                            perror("file_lock shared");
                            send_error(client_socket, "Error al bloquear la base de datos para lectura.");
                        }
                    } else {
                        process_command(client_socket, buffer, temp_db, &transaction_active);
                        file_unlock(temp_db);
                    }
                    fclose(temp_db);
                }
            }
        } else if (strcmp(buffer, "EXIT") == 0) {
            break;
        } else {
            send_error(client_socket, "Comando no reconocido.");
        }
    }

    if (transaction_active && db != NULL) {
        file_unlock(db);
        fclose(db);
    }
    close(client_socket);
}

// Helpers to send structured responses
void send_error(int client_socket, const char *msg) {
    char buf[BUFFER_SIZE];
    snprintf(buf, sizeof(buf), "ERROR;%s\n", msg);
    send(client_socket, buf, strlen(buf), 0);
}
void send_ok(int client_socket, const char *msg) {
    char buf[BUFFER_SIZE];
    snprintf(buf, sizeof(buf), "OK;%s\n", msg);
    send(client_socket, buf, strlen(buf), 0);
}

// (trim defined later)

// Process commands while holding either shared (non-transaction) or exclusive (transaction) lock
void process_command(int client_socket, const char *command, FILE *db, int *transaction_active) {

    if (strncmp(command, "QUERY;", 6) == 0) {
        const char *payload = command + 6; // may be empty
        fseek(db, 0, SEEK_SET);
        char line[BUFFER_SIZE];
        // send header
        if (fgets(line, sizeof(line), db)) {
            send(client_socket, line, strlen(line), 0);
        }
        while (fgets(line, sizeof(line), db)) {
            if (payload == NULL || strlen(payload) == 0) {
                send(client_socket, line, strlen(line), 0);
            } else {
                // Filtros simples: ID=, SKU=, CANTIDAD=, GENERATOR=
                char copy[BUFFER_SIZE];
                strncpy(copy, line, sizeof(copy)-1); copy[sizeof(copy)-1]=0;
                char *fields[4];
                int fi=0;
                char *tok = strtok(copy, ",");
                while (tok && fi < 4) { fields[fi++]=tok; tok = strtok(NULL, ","); }
                if (fi >= 4) {
                    char *id = trim(fields[0]);
                    char *sku = trim(fields[1]);
                    char *cantidad = trim(fields[2]);
                    char *generator = trim(fields[3]);
                    if (strncmp(payload, "ID=", 3) == 0) {
                        if (atol(payload+3) == atol(id)) send(client_socket, line, strlen(line), 0);
                    } else if (strncmp(payload, "SKU=", 4) == 0) {
                        if (strcmp(payload+4, sku) == 0) send(client_socket, line, strlen(line), 0);
                    } else if (strncmp(payload, "CANTIDAD=", 9) == 0) {
                        if (atol(payload+9) == atol(cantidad)) send(client_socket, line, strlen(line), 0);
                    } else if (strncmp(payload, "GENERATOR=", 10) == 0) {
                        if (atol(payload+10) == atol(generator)) send(client_socket, line, strlen(line), 0);
                    }
                }
            }
        }
        send_ok(client_socket, "Consulta finalizada.");

    } else if (strncmp(command, "INSERT;", 7) == 0) {
        if (!*transaction_active) { send_error(client_socket, "INSERT solo permitido en transacción."); return; }
        // payload: SKU,Cantidad,Generator
        const char *payload = command + 7;
        char copy[BUFFER_SIZE]; strncpy(copy, payload, sizeof(copy)-1); copy[sizeof(copy)-1]=0;
        // validar 3 campos
        char *p = copy; int commas = 0; for (; *p; ++p) if (*p == ',') commas++;
        if (commas < 2) { send_error(client_socket, "INSERT requiere: SKU,Cantidad,Generator"); return; }
        // compute next ID by scanning file
        fflush(db);
        fseek(db, 0, SEEK_SET);
        char line[BUFFER_SIZE];
        long maxid = 0;
        fgets(line, sizeof(line), db); // header
        while (fgets(line, sizeof(line), db)) {
            char *c = strdup(line);
            char *tok = strtok(c, ",");
            if (tok) { long id = atol(tok); if (id > maxid) maxid = id; }
            free(c);
        }
        long newid = maxid + 1;
        fseek(db, 0, SEEK_END);
        // write newline with newid + payload (already validated to be 3 fields)
        fprintf(db, "%ld,%s\n", newid, payload);
        fflush(db);
        send_ok(client_socket, "Registro insertado.");

    } else if (strncmp(command, "DELETE;", 7) == 0) {
        if (!*transaction_active) { send_error(client_socket, "DELETE solo permitido en transacción."); return; }
        const char *payload = command + 7; // id
        long idtodel = atol(payload);
        // read all, write to temp, skip id
        fflush(db);
        fseek(db,0,SEEK_SET);
        FILE *tmp = tmpfile();
        if (!tmp) { send_error(client_socket, "Error interno (tmpfile)."); return; }
        char line[BUFFER_SIZE];
        if (fgets(line, sizeof(line), db)) fputs(line, tmp); // header
        int found = 0;
        while (fgets(line, sizeof(line), db)) {
            char copy[BUFFER_SIZE]; strncpy(copy, line, sizeof(copy)-1); copy[sizeof(copy)-1]=0;
            char *tok = strtok(copy, ",");
            long id = atol(tok);
            if (id == idtodel) { found=1; continue; }
            fputs(line, tmp);
        }
        // rewind and overwrite db
        freopen(NULL, "w+", db); // reopen truncate
        rewind(tmp);
        while (fgets(line, sizeof(line), tmp)) fputs(line, db);
        fflush(db);
        fclose(tmp);
        if (found) send_ok(client_socket, "Registro eliminado."); else send_error(client_socket, "ID no encontrado.");

    } else if (strncmp(command, "UPDATE;", 7) == 0) {
        if (!*transaction_active) { send_error(client_socket, "UPDATE solo permitido en transacción."); return; }
        // payload: ID;field=value (e.g. UPDATE;12;Cantidad=50)
        const char *payload = command + 7;
        char copy[BUFFER_SIZE]; strncpy(copy, payload, sizeof(copy)-1); copy[sizeof(copy)-1]=0;
        char *p1 = strchr(copy, ';');
        if (!p1) { send_error(client_socket, "Payload inválido para UPDATE."); return; }
        *p1 = '\0';
        long idto = atol(copy);
        char *mod = p1+1; // field=value
        // read all, modify line with id
        fflush(db);
        fseek(db,0,SEEK_SET);
        FILE *tmp = tmpfile();
        if (!tmp) { send_error(client_socket, "Error interno (tmpfile)."); return; }
        char line[BUFFER_SIZE];
        if (fgets(line, sizeof(line), db)) fputs(line, tmp); // header
        int found = 0;
        while (fgets(line, sizeof(line), db)) {
            char copy2[BUFFER_SIZE]; strncpy(copy2, line, sizeof(copy2)-1); copy2[sizeof(copy2)-1]=0;
            char *fields[4]; int fi=0;
            char *tok = strtok(copy2, ",");
            while (tok && fi < 4) { fields[fi++]=tok; tok = strtok(NULL, ","); }
            if (fi >= 4) {
                long id = atol(fields[0]);
                if (id == idto) {
                    found = 1;
                    // rebuild line, apply modification (ID,SKU,Cantidad,Generator)
                    char sku[64]; int qty, gen;
                    strncpy(sku, trim(fields[1]), sizeof(sku)-1); sku[sizeof(sku)-1]=0;
                    qty = atoi(trim(fields[2])); gen = atoi(trim(fields[3]));
                    // parse mod
                    char *eq = strchr(mod, '=');
                    if (!eq) { fputs(line, tmp); continue; }
                    *eq = '\0';
                    char *field = mod; char *value = eq+1;
                    if (strcmp(field, "Cantidad") == 0) qty = atoi(value);
                    else if (strcmp(field, "SKU") == 0) strncpy(sku, value, sizeof(sku)-1);
                    else if (strcmp(field, "Generator") == 0) gen = atoi(value);
                    // write modified (4 columns)
                    fprintf(tmp, "%ld,%s,%d,%d\n", id, sku, qty, gen);
                } else {
                    fputs(line, tmp);
                }
            } else {
                fputs(line, tmp);
            }
        }
        // overwrite
        freopen(NULL, "w+", db);
        rewind(tmp);
        while (fgets(line, sizeof(line), tmp)) fputs(line, db);
        fflush(db);
        fclose(tmp);
        if (found) send_ok(client_socket, "Registro actualizado."); else send_error(client_socket, "ID no encontrado.");
    }
}

void read_config(const char *config_file, char *address_buf, int buf_size, int *port) {
    FILE *file = fopen(config_file, "r");
    if (file == NULL) {
        // No es un error si el archivo no existe, se usarán los defaults.
        return;
    }

    char line[100];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#' || line[0] == '\n') continue; // Ignorar comentarios y líneas vacías

        char key[50], value[100];
        if (sscanf(line, "%49[^=]=%99s", key, value) == 2) {
            if (strcmp(key, "ADDRESS") == 0) {
                strncpy(address_buf, value, buf_size - 1);
                address_buf[buf_size - 1] = '\0';
            } else if (strcmp(key, "PORT") == 0) {
                *port = atoi(value);
            }
        }
    }
    fclose(file);
}

void show_help(const char *name) {
    printf("Uso: %s -f <db_file> [-a <address>] [-p <port>] [-c <max_clients>] [-w <wait_queue>]\n", name);
    printf("El servidor leerá la configuración de red desde 'server.conf' si existe.\n");
    printf("Los parámetros de línea de comando tienen prioridad.\n");
    printf("\n");
    printf("  -h                Muestra esta ayuda.\n");
    printf("  -f <db_file>      Ruta al archivo CSV que se usará como base de datos.\n");
    printf("  -a <address>      Dirección IP para escuchar (default: 127.0.0.1).\n");
    printf("  -p <port>         Puerto para escuchar (default: 8080).\n");
    printf("  -c <max_clients>  Número máximo de clientes concurrentes (default: 5).\n");
    printf("  -w <wait_queue>   Tamaño de la cola de espera para conexiones (default: 10).\n");
}

void sigchld_handler(int s) {
    int saved_errno = errno;
    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        if (current_clients > 0) current_clients--;
        printf("Cliente (pid %d) finalizado. Clientes activos: %d\n", pid, current_clients);
    }
    // Wake main loop via self-pipe (ignore errors if full)
    if (g_sigpipe_fds[1] != -1) {
        char b = 'x';
        (void)write(g_sigpipe_fds[1], &b, 1);
    }
    errno = saved_errno;
}

// Trim implementation (single definition)
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) *end-- = '\0';
    return s;
}

static void sigterm_handler(int sig) {
    (void)sig;
    g_running = 0;
    if (g_server_fd != -1) {
        close(g_server_fd);
        g_server_fd = -1;
    }
}

// Promote queued sockets into active clients while slots available
static void promote_waiting(int *waitlist, int *w_count, int wait_queue,
                            volatile sig_atomic_t *cur_clients, int max_clients,
                            int server_fd, const char *db_file) {
    while (g_running && *w_count > 0 && *cur_clients < max_clients) {
        // pop front
        int sock = waitlist[0];
        if (*w_count > 1) memmove(&waitlist[0], &waitlist[1], sizeof(int) * (*w_count - 1));
        (*w_count)--;

    // Drain any pending input from the waiter to avoid treating it as a fresh command right after admission
    char drain[256];
    struct pollfd dfd = { .fd = sock, .events = POLLIN, .revents = 0 };
    while (poll(&dfd, 1, 0) > 0) { int n = recv(sock, drain, sizeof(drain), 0); if (n <= 0) break; }

        const char *admsg = "INFO;ADMITTED\n";
        (void)send(sock, admsg, strlen(admsg), 0);

        int lifeline[2];
        if (pipe(lifeline) == -1) { perror("pipe lifeline"); lifeline[0] = lifeline[1] = -1; }

        sigset_t block, prev;
        sigemptyset(&block); sigaddset(&block, SIGCHLD);
        if (sigprocmask(SIG_BLOCK, &block, &prev) == -1) perror("sigprocmask block");

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork failed");
            close(sock);
            if (lifeline[0] != -1) close(lifeline[0]);
            if (lifeline[1] != -1) close(lifeline[1]);
        } else if (pid == 0) {
            close(server_fd);
            if (lifeline[1] != -1) close(lifeline[1]);
#ifdef __linux__
            prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
            if (sigprocmask(SIG_SETMASK, &prev, NULL) == -1) perror("sigprocmask restore child");
            handle_client(sock, db_file, lifeline[0]);
            exit(EXIT_SUCCESS);
        } else {
            if (lifeline[0] != -1) close(lifeline[0]);
            (*cur_clients)++;
            if (sigprocmask(SIG_SETMASK, &prev, NULL) == -1) perror("sigprocmask restore");
        }
    }
}

static void send_wait_positions(int *waitlist, int w_count, int wait_queue) {
    for (int i = 0; i < w_count; ++i) {
        int pos = i + 1;
        char msg[128];
        snprintf(msg, sizeof(msg), "INFO;WAIT;pos=%d cap=%d\n", pos, wait_queue);
        (void)send(waitlist[i], msg, strlen(msg), 0);
    }
}
