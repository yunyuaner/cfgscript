#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cfg cfg_t;

typedef enum {
    CFG_OK = 0,
    CFG_ERR_IO = -1,
    CFG_ERR_PARSE = -2,
    CFG_ERR_OOM = -3,
    CFG_ERR_RECURSION = -4
} cfg_status_t;

/* Load config (preprocess + parse). On failure returns NULL and sets out_status. */
cfg_t* cfg_load(const char* path, cfg_status_t* out_status);

/* Get last error message (static buffer, overwritten by next cfg_load). */
const char* cfg_last_error(void);

void cfg_free(cfg_t* c);

int         cfg_has(const cfg_t* c, const char* key);
const char* cfg_get_str(const cfg_t* c, const char* key, const char* defval);
long long   cfg_get_int(const cfg_t* c, const char* key, long long defval);
int         cfg_get_bool(const cfg_t* c, const char* key, int defval);

void cfg_dump(const cfg_t* c);

int cfg_get_origin(const cfg_t* c, const char* key, const char** out_file, int* out_line);

/* Dump preprocessed (expanded) config to a file.
 * - Input: original cfg file with directives.
 * - Output: plain KV text after %define/%if/%for/%include expansion (directives removed).
 * - with_origin: if non-zero, emit origin comments like "# file:line" before each line.
 */
cfg_status_t cfg_dump_preprocessed_file(const char* in_path,
                                        const char* out_path,
                                        int with_origin);

/* Dump preprocessed config into a malloc()'d string (caller frees). */
cfg_status_t cfg_dump_preprocessed_text(const char* in_path,
                                        char** out_text,
                                        size_t* out_len,
                                        int with_origin);

#ifdef __cplusplus
}
#endif
