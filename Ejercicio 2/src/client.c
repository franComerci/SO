// Habilita prototipos POSIX (getopt)
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
    fprintf(stderr, "\n[CLIENT] Señal recibida. Cerrando conexión...\n");
}

#define BUFFER_SIZE 8192

void read_config(const char *config_file, char *address_buf, int buf_size, int *port);
void show_help(const char *name);
void communicate_with_server(int sock);

int main(int argc, char *argv[]) {
    // Señales para salida ordenada
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    int opt;
    char address_buf[40] = "127.0.0.1";
    char *address = address_buf;
    int port = 8080;

    // Leer configuración desde archivo primero
    read_config("server.conf", address_buf, sizeof(address_buf), &port);
    address = address_buf;

    while ((opt = getopt(argc, argv, "ha:p:")) != -1) {
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
            default:
                show_help(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    int sock = 0;
    struct sockaddr_in serv_addr;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, address, &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "Dirección inválida o no soportada: %s\n", address);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        return -1;
    }

    printf("Conectado al servidor en %s:%d\n", address, port);
    printf("Puede empezar a enviar comandos. Escriba 'HELP' para ver los comandos disponibles o 'EXIT' para salir.\n");

    communicate_with_server(sock);

    close(sock);
    return 0;
}

static void send_and_print_response(int sock, const char *command) {
    if (send(sock, command, strlen(command), 0) < 0) {
        fprintf(stderr, "[CLIENT] Error enviando comando: %s\n", strerror(errno));
        return;
    }

    if (strcmp(command, "EXIT") == 0) {
        printf("Desconectando del servidor.\n");
        return;
    }

    char buffer[BUFFER_SIZE];
    printf("Respuesta del servidor:\n");
    int saw_terminator = 0;
    // Leer trozos disponibles sin bloquear indefinidamente.
    for (;;) {
        struct pollfd pfd = { .fd = sock, .events = POLLIN | POLLHUP | POLLERR, .revents = 0 };
        int pr = poll(&pfd, 1, 500); // hasta 500ms entre trozos
        if (pr < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[CLIENT] Error esperando respuesta: %s\n", strerror(errno));
            break;
        }
        if (pr == 0) {
            // timeout: si no hubo terminador, salimos para no colgar (ej: INFO;WAIT)
            break;
        }
        if (pfd.revents & (POLLHUP | POLLERR)) {
            fprintf(stderr, "[CLIENT] Servidor no disponible.\n");
            break;
        }
        int bytes_read = recv(sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_read <= 0) break;
        buffer[bytes_read] = '\0';
        printf("%s", buffer);
        if (strstr(buffer, "OK;") || strstr(buffer, "ERROR;")) { saw_terminator = 1; break; }
        if (strstr(buffer, "INFO;")) {
            // Info banners (WAIT/ADMITTED) no tienen terminador: no bloquear
            break;
        }
    }
    (void)saw_terminator; // reservado por si se usa en el futuro
}

static void show_menu(void) {
    printf("\n=== MENU (modo asistido) ===\n");
    printf("1) QUERY; (listar todo)\n");
    printf("2) QUERY con filtro (ID, SKU, CANTIDAD, GENERATOR)\n");
    printf("3) BEGIN_TRANSACTION\n");
    printf("4) INSERT;SKU,Cantidad,Generator\n");
    printf("5) UPDATE;ID;Field=Value   (Field: SKU|Cantidad|Generator)\n");
    printf("6) DELETE;ID\n");
    printf("7) COMMIT_TRANSACTION\n");
    printf("8) EXIT (cerrar conexión)\n");
    printf("9) BACK (volver a modo libre)\n");
}

static void guided_mode(int sock) {
    char line[BUFFER_SIZE];
    for (;;) {
        show_menu();
        printf("Seleccione opción [1-9]: ");
        if (!fgets(line, sizeof(line), stdin)) return;
        int opt = atoi(line);

        char cmd[BUFFER_SIZE];
        memset(cmd, 0, sizeof(cmd));

        if (opt == 1) {
            strcpy(cmd, "QUERY;");
        } else if (opt == 2) {
            printf("Campo (ID|SKU|CANTIDAD|GENERATOR): ");
            if (!fgets(line, sizeof(line), stdin)) return;
            line[strcspn(line, "\n")] = 0;
            char field[32]; strncpy(field, line, sizeof(field)-1); field[sizeof(field)-1]=0;
            printf("Valor: ");
            if (!fgets(line, sizeof(line), stdin)) return;
            line[strcspn(line, "\n")] = 0;
            char val[512]; strncpy(val, line, sizeof(val)-1); val[sizeof(val)-1]=0;
            snprintf(cmd, sizeof(cmd), "QUERY;%s=%s", field, val);
        } else if (opt == 3) {
            strcpy(cmd, "BEGIN_TRANSACTION");
        } else if (opt == 4) {
            char sku[128]; char qty[32]; char gen[32];
            printf("SKU: ");
            if (!fgets(sku, sizeof(sku), stdin)) return;
            sku[strcspn(sku, "\n")] = 0;
            printf("Cantidad (int): ");
            if (!fgets(qty, sizeof(qty), stdin)) return;
            qty[strcspn(qty, "\n")] = 0;
            printf("Generator (int): ");
            if (!fgets(gen, sizeof(gen), stdin)) return;
            gen[strcspn(gen, "\n")] = 0;
            snprintf(cmd, sizeof(cmd), "INSERT;%s,%s,%s", sku, qty, gen);
        } else if (opt == 5) {
            char id[32]; char field[64]; char value[256];
            printf("ID: ");
            if (!fgets(id, sizeof(id), stdin)) return;
            id[strcspn(id, "\n")] = 0;
            printf("Field (SKU|Cantidad|Generator): ");
            if (!fgets(field, sizeof(field), stdin)) return;
            field[strcspn(field, "\n")] = 0;
            printf("Value: ");
            if (!fgets(value, sizeof(value), stdin)) return;
            value[strcspn(value, "\n")] = 0;
            snprintf(cmd, sizeof(cmd), "UPDATE;%s;%s=%s", id, field, value);
        } else if (opt == 6) {
            char id[32];
            printf("ID: ");
            if (!fgets(id, sizeof(id), stdin)) return;
            id[strcspn(id, "\n")] = 0;
            snprintf(cmd, sizeof(cmd), "DELETE;%s", id);
        } else if (opt == 7) {
            strcpy(cmd, "COMMIT_TRANSACTION");
        } else if (opt == 8) {
            strcpy(cmd, "EXIT");
        } else if (opt == 9) {
            printf("Volviendo a modo libre.\n\n");
            return;
        } else {
            printf("Opción inválida.\n");
            continue;
        }

        send_and_print_response(sock, cmd);
        if (strcmp(cmd, "EXIT") == 0) return;
    }
}

void communicate_with_server(int sock) {
    char command[BUFFER_SIZE] = {0};
    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO; fds[0].events = POLLIN; fds[0].revents = 0;
    fds[1].fd = sock; fds[1].events = POLLIN | POLLHUP | POLLERR; fds[1].revents = 0;

    while (1) {
        printf("> (escriba HELP o MENU) ");
        fflush(stdout);
        int pr = poll(fds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (fds[1].revents & (POLLIN)) {
            char buf[BUFFER_SIZE];
            int n = recv(sock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) {
                fprintf(stderr, "\n[CLIENT] Servidor no disponible. Cerrando.\n");
                break;
            }
            buf[n] = '\0';
            // Print any server-side INFO/ERROR immediately
            printf("\n%s", buf);
            // If server sent an ERROR;Servidor ocupado..., the server should close soon; keep loop to catch HUP.
            continue;
        }
        if (fds[1].revents & (POLLHUP | POLLERR)) {
            fprintf(stderr, "\n[CLIENT] Servidor no disponible. Cerrando.\n");
            break;
        }
        if (!(fds[0].revents & POLLIN)) continue;
        if (!fgets(command, BUFFER_SIZE, stdin)) break;
        command[strcspn(command, "\n")] = 0; // Remove newline

        if (strcmp(command, "HELP") == 0) {
            printf("Comandos disponibles:\n");
            printf("  QUERY; - Devuelve todo el inventario.\n");
            printf("  QUERY;ID=123 - Busca por ID.\n");
            printf("  QUERY;SKU=SKU000123 - Busca por SKU.\n");
            printf("  QUERY;CANTIDAD=50 - Filtra por cantidad.\n");
            printf("  BEGIN_TRANSACTION - Inicia una transacción.\n");
            printf("  INSERT;SKU,Cantidad,Generator - Inserta un producto (transacción requerida).\n");
            printf("  DELETE;ID - Elimina un producto (transacción requerida).\n");
            printf("  UPDATE;ID;Field=Value - Actualiza campo (SKU, Cantidad, Generator).\n");
            printf("  COMMIT_TRANSACTION - Confirma cambios.\n");
            printf("  MENU - Modo asistido con opciones y prompts.\n");
            printf("  EXIT - Cierra la conexión.\n");
            continue;
        }

        if (strcmp(command, "MENU") == 0 || strcmp(command, "menu") == 0) {
            guided_mode(sock);
            continue;
        }

        send_and_print_response(sock, command);
        if (strcmp(command, "EXIT") == 0) break;
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

        char key[50], value[50];
        if (sscanf(line, "%49[^=]=%49s", key, value) == 2) {
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
    printf("Uso: %s [-a <address>] [-p <port>]\n", name);
    printf("El cliente leerá la configuración de red desde 'server.conf' si existe.\n");
    printf("Los parámetros de línea de comando tienen prioridad.\n");
    printf("\n");
    printf("  -h           Muestra esta ayuda.\n");
    printf("  -a <address> Dirección IP del servidor (default: 127.0.0.1).\n");
    printf("  -p <port>    Puerto del servidor (default: 8080).\n");
}
