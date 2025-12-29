#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>

#define COPY_BUF 65536
#define MAX_THREADS 64

typedef struct {
    char src[PATH_MAX];
    char dst[PATH_MAX];
} task_t;

static int safe_lstat(const char *path, struct stat *st) {
    while (lstat(path, st) < 0) {
        if (errno == EMFILE || errno == EAGAIN) { 
            sleep(1); 
            continue; 
        }
        fprintf(stderr, "[DEBUG] cannot lstat: %s\n", path);
        perror("lstat");
        return -1;
    }
    return 0;
}

void *copy_file(void *arg) {
    task_t *t = arg;
    int in_fd = -1, out_fd = -1;
    char buf[COPY_BUF];
    ssize_t r;

    while ((in_fd = open(t->src, O_RDONLY)) < 0) {
        if (errno == EMFILE || errno == EAGAIN) { 
            sleep(1); 
            continue; 
        }
        fprintf(stderr, "[DEBUG] cannot open source file: %s\n", t->src);
        perror("open src");
        goto done;
    }

    struct stat st;
    if (safe_lstat(t->src, &st) < 0) goto done;

    while ((out_fd = open(t->dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode)) < 0) {
        if (errno == EMFILE || errno == EAGAIN) { 
            sleep(1); 
            continue; 
        }
        fprintf(stderr, "[DEBUG] cannot open destination file: %s\n", t->dst);
        perror("open dst");
        goto done;
    }

    while ((r = read(in_fd, buf, COPY_BUF)) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out_fd, buf + off, r - off);
            if (w < 0) {
                perror("write");
                goto done;
            }
            off += w;
        }
    }
    if (r < 0) 
        perror("read");

done:
    if (in_fd >= 0) close(in_fd);
    if (out_fd >= 0) close(out_fd);
    free(t);
    return NULL;
}


void *copy_dir(void *arg) {
    task_t *t = arg;
    DIR *dir = NULL;

    while (!(dir = opendir(t->src))) {
        if (errno == EMFILE || errno == EAGAIN) { 
            sleep(1); 
            continue; 
        }
        fprintf(stderr, "[DEBUG] cannot open directory: %s\n", t->src);
        perror("opendir");
        goto done;
    }

    if (mkdir(t->dst, 0755) < 0 && errno != EEXIST) {
        perror("mkdir");
        goto done;
    }
    long name_max = pathconf(t->src, _PC_NAME_MAX);
    if (name_max < 0) name_max = 255;
    size_t entry_size = sizeof(struct dirent) + name_max + 1;
    struct dirent *entry_buf = malloc(entry_size);
    if (!entry_buf) { 
        perror("malloc"); 
        goto done; 
    }

    struct dirent *result;
    pthread_t threads[MAX_THREADS];
    int n_threads = 0;

    while (1) {
        int ret = readdir_r(dir, entry_buf, &result);
        if (ret != 0) { 
            errno = ret; 
            perror("readdir_r"); 
            break; 
        }
        if (result == NULL) 
            break;

        if (!strcmp(result->d_name, ".") || !strcmp(result->d_name, ".."))
            continue;

        char src_path[PATH_MAX], dst_path[PATH_MAX];

        if (snprintf(src_path, PATH_MAX, "%s/%s", t->src, result->d_name) >= PATH_MAX ||
            snprintf(dst_path, PATH_MAX, "%s/%s", t->dst, result->d_name) >= PATH_MAX) {
            fprintf(stderr, "[DEBUG] path too long: %s/%s\n", t->src, result->d_name);
            continue;
        }

        struct stat st;
        if (safe_lstat(src_path, &st) < 0) 
            continue;

        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
            fprintf(stderr, "[DEBUG] ignoring non-regular file: %s\n", src_path);
            continue;
        }

        task_t *nt = malloc(sizeof(task_t));
        if (!nt) { 
            perror("malloc"); 
            continue; 
        }
        strcpy(nt->src, src_path);
        strcpy(nt->dst, dst_path);

        int prc;
        if (S_ISDIR(st.st_mode)) {
            prc = pthread_create(&threads[n_threads], NULL, copy_dir, nt);
        } else {
            prc = pthread_create(&threads[n_threads], NULL, copy_file, nt);
        }

        if (prc != 0) {
            errno = prc;
            perror("pthread_create");
            free(nt);
            continue;
        }

        n_threads++;
        if (n_threads == MAX_THREADS) {
            for (int i = 0; i < n_threads; i++)
                pthread_join(threads[i], NULL);
            n_threads = 0;
        }
    }

    for (int i = 0; i < n_threads; i++)
        pthread_join(threads[i], NULL);

    free(entry_buf);
    closedir(dir);

done:
    free(t);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s SRC DST\n", argv[0]);
        return 1;
    }

    char src_real[PATH_MAX];
    if (!realpath(argv[1], src_real)) { 
        perror("realpath src"); 
        return 1; 
    }

    char dst_real[PATH_MAX];
    strncpy(dst_real, argv[2], PATH_MAX - 1);
    dst_real[PATH_MAX - 1] = '\0';

    size_t src_len = strlen(src_real);
    if (strncmp(dst_real, src_real, src_len) == 0 &&
        (dst_real[src_len] == '/' || dst_real[src_len] == '\0')) {
        fprintf(stderr, "error: destination directory is inside source\n");
        return 1;
    }

    struct stat st;
    if (lstat(src_real, &st) < 0) { 
        perror("stat src"); 
        return 1; 
    }

    task_t *t = malloc(sizeof(task_t));
    if (!t) { 
        perror("malloc"); 
        return 1; 
    }
    strcpy(t->src, src_real);
    strcpy(t->dst, dst_real);

    pthread_t tid;
    int rc;

    if (S_ISDIR(st.st_mode)) {
        rc = pthread_create(&tid, NULL, copy_dir, t);
    } else if (S_ISREG(st.st_mode)) {
        rc = pthread_create(&tid, NULL, copy_file, t);
    } else {
        fprintf(stderr, "source is neither file nor directory\n");
        free(t);
        return 1;
    }

    if (rc != 0) { 
        errno = rc; 
        perror("pthread_create"); 
        free(t); 
        return 1; 
    }

    pthread_join(tid, NULL);
    return 0;
}
