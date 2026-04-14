/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

static volatile sig_atomic_t supervisor_running = 1;

static void supervisor_signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM)
        supervisor_running = 0;
}

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    mkdir(LOG_DIR, 0755);

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        FILE *fp;

        if (item.container_id[0] == '\0' || item.length == 0)
            continue;

        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        fp = fopen(path, "a");
        if (!fp)
            continue;

        fwrite(item.data, 1, item.length, fp);
        fclose(fp);
    }

    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    int null_fd;

    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    if (chdir(cfg->rootfs) != 0)
        return 1;

    /* Chroot to the current directory after chdir so relative rootfs paths work. */
    if (chroot(".") != 0)
        return 1;
    if (chdir("/") != 0)
        return 1;

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        close(null_fd);
    }

    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    execl("/bin/sh", "sh", "-c", cfg->command, NULL);
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
    struct sockaddr_un addr;
    struct sigaction sa;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    mkdir(LOG_DIR, 0755);
    printf("Supervisor mode started for base-rootfs: %s\n", rootfs);

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }

    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen");
        goto cleanup;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = supervisor_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create");
        goto cleanup;
    }

    while (supervisor_running) {
        int client_fd;
        control_request_t req;
        ssize_t nread;
        container_record_t *cur;

        while (1) {
            int wstatus;
            pid_t done = waitpid(-1, &wstatus, WNOHANG);
            if (done <= 0)
                break;

            pthread_mutex_lock(&ctx.metadata_lock);
            cur = ctx.containers;
            while (cur) {
                if (cur->host_pid == done) {
                    if (WIFEXITED(wstatus)) {
                        cur->state = CONTAINER_EXITED;
                        cur->exit_code = WEXITSTATUS(wstatus);
                        cur->exit_signal = 0;
                    } else if (WIFSIGNALED(wstatus)) {
                        cur->state = CONTAINER_KILLED;
                        cur->exit_signal = WTERMSIG(wstatus);
                        cur->exit_code = 128 + cur->exit_signal;
                    }
                    if (ctx.monitor_fd >= 0)
                        unregister_from_monitor(ctx.monitor_fd, cur->id, cur->host_pid);
                    break;
                }
                cur = cur->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
        }

        client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            continue;
        }

        nread = recv(client_fd, &req, sizeof(req), 0);
        if (nread <= 0) {
            close(client_fd);
            continue;
        }

        if (req.kind == CMD_START || req.kind == CMD_RUN) {
            container_record_t *rec = NULL;
            child_config_t *cfg = NULL;
            char *stack = NULL;
            int log_fd = -1;
            pid_t pid;

            pthread_mutex_lock(&ctx.metadata_lock);
            cur = ctx.containers;
            while (cur) {
                if (strcmp(cur->id, req.container_id) == 0 &&
                    (cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING)) {
                    break;
                }
                cur = cur->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);

            if (cur) {
                dprintf(client_fd, "ERROR: container id already running\n");
                close(client_fd);
                continue;
            }

            rec = calloc(1, sizeof(*rec));
            cfg = calloc(1, sizeof(*cfg));
            stack = malloc(STACK_SIZE);
            if (!rec || !cfg || !stack) {
                free(rec);
                free(cfg);
                free(stack);
                dprintf(client_fd, "ERROR: allocation failed\n");
                close(client_fd);
                continue;
            }

            strncpy(rec->id, req.container_id, sizeof(rec->id) - 1);
            rec->started_at = time(NULL);
            rec->state = CONTAINER_STARTING;
            rec->soft_limit_bytes = req.soft_limit_bytes;
            rec->hard_limit_bytes = req.hard_limit_bytes;
            snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, req.container_id);

            strncpy(cfg->id, req.container_id, sizeof(cfg->id) - 1);
            strncpy(cfg->rootfs, req.rootfs, sizeof(cfg->rootfs) - 1);
            strncpy(cfg->command, req.command, sizeof(cfg->command) - 1);
            cfg->nice_value = req.nice_value;

            log_fd = open(rec->log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
            cfg->log_write_fd = log_fd;

            pid = clone(child_fn,
                        stack + STACK_SIZE,
                        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                        cfg);
            if (pid < 0) {
                perror("clone");
                if (log_fd >= 0)
                    close(log_fd);
                free(rec);
                free(cfg);
                free(stack);
                dprintf(client_fd, "ERROR: clone failed\n");
                close(client_fd);
                continue;
            }

            if (log_fd >= 0)
                close(log_fd);
            free(cfg);
            free(stack);

            rec->host_pid = pid;
            rec->state = CONTAINER_RUNNING;

            pthread_mutex_lock(&ctx.metadata_lock);
            rec->next = ctx.containers;
            ctx.containers = rec;
            pthread_mutex_unlock(&ctx.metadata_lock);

            if (ctx.monitor_fd >= 0)
                register_with_monitor(ctx.monitor_fd, rec->id, rec->host_pid, rec->soft_limit_bytes, rec->hard_limit_bytes);

            dprintf(client_fd, "OK: started %s pid=%d\n", rec->id, rec->host_pid);

            if (req.kind == CMD_RUN) {
                int wstatus;
                if (waitpid(pid, &wstatus, 0) > 0) {
                    pthread_mutex_lock(&ctx.metadata_lock);
                    rec->state = WIFSIGNALED(wstatus) ? CONTAINER_KILLED : CONTAINER_EXITED;
                    rec->exit_signal = WIFSIGNALED(wstatus) ? WTERMSIG(wstatus) : 0;
                    rec->exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus)
                                                        : (128 + rec->exit_signal);
                    pthread_mutex_unlock(&ctx.metadata_lock);

                    if (ctx.monitor_fd >= 0)
                        unregister_from_monitor(ctx.monitor_fd, rec->id, rec->host_pid);
                }
                dprintf(client_fd, "RUN_RESULT id=%s exit_code=%d\n", rec->id, rec->exit_code);
            }
        } else if (req.kind == CMD_PS) {
            pthread_mutex_lock(&ctx.metadata_lock);
            cur = ctx.containers;
            while (cur) {
                dprintf(client_fd,
                        "ID=%s PID=%d STATE=%s SOFT_MIB=%lu HARD_MIB=%lu EXIT=%d\n",
                        cur->id,
                        cur->host_pid,
                        state_to_string(cur->state),
                        cur->soft_limit_bytes >> 20,
                        cur->hard_limit_bytes >> 20,
                        cur->exit_code);
                cur = cur->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
        } else if (req.kind == CMD_LOGS) {
            char path[PATH_MAX];
            FILE *fp;
            char line[512];

            snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);
            fp = fopen(path, "r");
            if (!fp) {
                dprintf(client_fd, "ERROR: log file not found\n");
            } else {
                while (fgets(line, sizeof(line), fp))
                    dprintf(client_fd, "%s", line);
                fclose(fp);
            }
        } else if (req.kind == CMD_STOP) {
            int found = 0;
            pthread_mutex_lock(&ctx.metadata_lock);
            cur = ctx.containers;
            while (cur) {
                if (strcmp(cur->id, req.container_id) == 0) {
                    found = 1;
                    if (cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING) {
                        kill(cur->host_pid, SIGTERM);
                        kill(cur->host_pid, SIGKILL);
                        cur->state = CONTAINER_STOPPED;
                    }
                    break;
                }
                cur = cur->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            dprintf(client_fd, found ? "OK: stop issued\n" : "ERROR: container not found\n");
        } else {
            dprintf(client_fd, "ERROR: unsupported command\n");
        }

        close(client_fd);
    }

cleanup:
    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    unlink(CONTROL_PATH);

    if (ctx.logger_thread) {
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        pthread_join(ctx.logger_thread, NULL);
    }

    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *cur = ctx.containers;
        while (cur) {
            container_record_t *next = cur->next;
            if (cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING)
                kill(cur->host_pid, SIGKILL);
            free(cur);
            cur = next;
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    char buf[512];
    ssize_t n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) < 0) {
        perror("send");
        close(fd);
        return 1;
    }

    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(STDOUT_FILENO, buf, n);

    close(fd);
    return 0;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
