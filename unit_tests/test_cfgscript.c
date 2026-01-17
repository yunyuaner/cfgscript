#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/types.h>

#include "../cfgscript.h"

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "ASSERT_TRUE failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 0; \
    } \
} while(0)

#define ASSERT_EQ_INT(a,b) do { \
    long long _aa=(a), _bb=(b); \
    if (_aa != _bb) { \
        fprintf(stderr, "ASSERT_EQ_INT failed at %s:%d: %" PRId64 " != %" PRId64 "\n", __FILE__, __LINE__, _aa, _bb); \
        return 0; \
    } \
} while(0)

#define ASSERT_EQ_STR(a,b) do { \
    const char* _aa=(a); const char* _bb=(b); \
    if ((_aa==NULL && _bb!=NULL) || (_aa!=NULL && _bb==NULL) || (_aa && _bb && strcmp(_aa,_bb)!=0)) { \
        fprintf(stderr, "ASSERT_EQ_STR failed at %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _aa?_aa:"(null)", _bb?_bb:"(null)"); \
        return 0; \
    } \
} while(0)

static int load_ok(const char* path, cfg_t** out_cfg) {
    cfg_status_t st;
    cfg_t* c = cfg_load(path, &st);
    if (!c) {
        fprintf(stderr, "cfg_load failed for %s, st=%d, err=%s\n", path, (int)st, cfg_last_error());
        return 0;
    }
    *out_cfg = c;
    return 1;
}

static int load_fail(const char* path, cfg_status_t expect_status, const char* expect_substr) {
    cfg_status_t st;
    cfg_t* c = cfg_load(path, &st);
    if (c) {
        fprintf(stderr, "expected failure but cfg_load succeeded for %s\n", path);
        cfg_free(c);
        return 0;
    }
    if (st != expect_status) {
        fprintf(stderr, "expected status %d but got %d for %s (err=%s)\n",
                (int)expect_status, (int)st, path, cfg_last_error());
        return 0;
    }
    if (expect_substr && expect_substr[0]) {
        if (!strstr(cfg_last_error(), expect_substr)) {
            fprintf(stderr, "expected error to contain \"%s\" but got \"%s\"\n",
                    expect_substr, cfg_last_error());
            return 0;
        }
    }
    return 1;
}

static char* read_file_all(const char* path, size_t* out_n) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = 0;
    if (out_n) *out_n = n;
    return buf;
}

/* ---------------- tests ---------------- */

static int test_basic_macros(const char* path) {
    cfg_t* c=NULL;
    ASSERT_TRUE(load_ok(path, &c));
    ASSERT_EQ_STR(cfg_get_str(c, "server.host", ""), "localhost");
    ASSERT_EQ_INT(cfg_get_int(c, "server.port", 0), 8080);
    ASSERT_EQ_INT(cfg_get_bool(c, "server.enable", 0), 1);

    /* origin should be a stable string pointer */
    const char* f=NULL; int line=0;
    ASSERT_TRUE(cfg_get_origin(c, "server.port", &f, &line));
    ASSERT_TRUE(f != NULL);
    ASSERT_TRUE(strstr(f, "basic_macros.cfg") != NULL);
    ASSERT_TRUE(line > 0);

    cfg_free(c);
    return 1;
}

static int test_if_elif_else(const char* path) {
    cfg_t* c=NULL;
    ASSERT_TRUE(load_ok(path, &c));
    ASSERT_EQ_STR(cfg_get_str(c, "app.profile", ""), "production");
    cfg_free(c);
    return 1;
}

static int test_set_expr_and_bool(const char* path) {
    cfg_t* c=NULL;
    ASSERT_TRUE(load_ok(path, &c));
    ASSERT_EQ_INT(cfg_get_int(c, "value.c", 0), 16);
    ASSERT_EQ_INT(cfg_get_int(c, "value.d", 0), 11);
    ASSERT_EQ_INT(cfg_get_bool(c, "ok", 0), 1);
    cfg_free(c);
    return 1;
}

static int test_for_range(const char* path) {
    cfg_t* c=NULL;
    ASSERT_TRUE(load_ok(path, &c));
    ASSERT_EQ_INT(cfg_get_int(c, "node.1.port", 0), 8001);
    ASSERT_EQ_INT(cfg_get_int(c, "node.2.port", 0), 8002);
    ASSERT_EQ_INT(cfg_get_int(c, "node.3.port", 0), 8003);
    ASSERT_EQ_INT(cfg_get_bool(c, "node.2.enable", 0), 1);
    cfg_free(c);
    return 1;
}

static int test_for_list(const char* path) {
    cfg_t* c=NULL;
    ASSERT_TRUE(load_ok(path, &c));
    ASSERT_EQ_INT(cfg_get_bool(c, "user.alice.enable", 0), 1);
    ASSERT_EQ_INT(cfg_get_bool(c, "user.bob.enable", 0), 1);
    ASSERT_EQ_INT(cfg_get_bool(c, "user.carol.enable", 0), 1);
    cfg_free(c);
    return 1;
}

static int test_nested_for(const char* path) {
    cfg_t* c=NULL;
    ASSERT_TRUE(load_ok(path, &c));
    ASSERT_EQ_STR(cfg_get_str(c, "k.1.1", ""), "11");
    ASSERT_EQ_STR(cfg_get_str(c, "k.1.3", ""), "13");
    ASSERT_EQ_STR(cfg_get_str(c, "k.2.2", ""), "22");
    ASSERT_TRUE(!cfg_has(c, "k.3.1"));
    cfg_free(c);
    return 1;
}

static int test_include(const char* path) {
    cfg_t* c=NULL;
    ASSERT_TRUE(load_ok(path, &c));
    ASSERT_EQ_INT(cfg_get_int(c, "sum", 0), 112);
    cfg_free(c);
    return 1;
}

/* NEW: dump preprocessed text/file */
static int test_dump_preprocessed(const char* dir) {
    char in_path[1024];
    snprintf(in_path, sizeof(in_path), "%s/dump_target.cfg", dir);

    /* dump to text */
    char* txt = NULL;
    size_t n = 0;
    cfg_status_t st = cfg_dump_preprocessed_text(in_path, &txt, &n, 0);
    ASSERT_TRUE(st == CFG_OK);
    ASSERT_TRUE(txt != NULL);

    /* should not contain directives */
    ASSERT_TRUE(strstr(txt, "%define") == NULL);
    ASSERT_TRUE(strstr(txt, "%include") == NULL);
    ASSERT_TRUE(strstr(txt, "%for") == NULL);
    ASSERT_TRUE(strstr(txt, "%if") == NULL);

    /* should contain expanded KV lines */
    ASSERT_TRUE(strstr(txt, "inc.key = included") != NULL);
    ASSERT_TRUE(strstr(txt, "server.host = example.com") != NULL);
    ASSERT_TRUE(strstr(txt, "server.port = 9000") != NULL);
    ASSERT_TRUE(strstr(txt, "node.1.name = node1") != NULL);
    ASSERT_TRUE(strstr(txt, "node.2.name = node2") != NULL);
    ASSERT_TRUE(strstr(txt, "extra.enabled = true") != NULL);

    free(txt);

    /* dump to text with origin */
    txt = NULL; n = 0;
    st = cfg_dump_preprocessed_text(in_path, &txt, &n, 1);
    ASSERT_TRUE(st == CFG_OK);
    ASSERT_TRUE(txt != NULL);
    /* origin comments */
    ASSERT_TRUE(strstr(txt, "# ") != NULL);
    ASSERT_TRUE(strstr(txt, "dump_target.cfg:") != NULL);
    free(txt);

    /* dump to file */
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/_expanded_out.cfg", dir);

    st = cfg_dump_preprocessed_file(in_path, out_path, 0);
    ASSERT_TRUE(st == CFG_OK);

    size_t fn = 0;
    char* ftxt = read_file_all(out_path, &fn);
    ASSERT_TRUE(ftxt != NULL);
    ASSERT_TRUE(strstr(ftxt, "server.host = example.com") != NULL);
    ASSERT_TRUE(strstr(ftxt, "%define") == NULL);
    free(ftxt);

    return 1;
}

static int test_errors(const char* dir) {
    char p[1024];

    snprintf(p, sizeof(p), "%s/err_div0.cfg", dir);
    ASSERT_TRUE(load_fail(p, CFG_ERR_PARSE, "division by zero"));

    snprintf(p, sizeof(p), "%s/err_missing_endif.cfg", dir);
    ASSERT_TRUE(load_fail(p, CFG_ERR_PARSE, "missing %endif"));

    snprintf(p, sizeof(p), "%s/err_unexpected_endfor.cfg", dir);
    ASSERT_TRUE(load_fail(p, CFG_ERR_PARSE, "unexpected %endfor"));

    snprintf(p, sizeof(p), "%s/err_unterminated_for.cfg", dir);
    ASSERT_TRUE(load_fail(p, CFG_ERR_PARSE, "unterminated %for"));

    return 1;
}

static int run1(const char* name, int ok) {
    printf("[%-24s] %s\n", name, ok ? "OK" : "FAIL");
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: test_cfgscript <config_dir>\n");
        return 2;
    }
    const char *dir = argv[1];
    char path[1024];
    int pass = 1;

    DIR* dp = opendir(dir);
    if (!dp) {
        fprintf(stderr, "Failed to open dir: %s\n", dir);
        return 2;
    }

    int ran_errors = 0;
    int ran_dump = 0;

    struct dirent* ent;
    while ((ent = readdir(dp)) != NULL) {
        const char* name = ent->d_name;
        size_t nl = strlen(name);
        if (nl < 5) continue; /* skip short names */
        if (strcmp(name + nl - 4, ".cfg") != 0) continue;

        snprintf(path, sizeof(path), "%s/%s", dir, name);

        if (strcmp(name, "basic_macros.cfg") == 0) {
            pass &= run1("basic_macros", test_basic_macros(path));
            continue;
        }
        if (strcmp(name, "if_elif_else.cfg") == 0) {
            pass &= run1("if_elif_else", test_if_elif_else(path));
            continue;
        }
        if (strcmp(name, "set_expr_and_bool.cfg") == 0) {
            pass &= run1("set_expr_and_bool", test_set_expr_and_bool(path));
            continue;
        }
        if (strcmp(name, "for_range.cfg") == 0) {
            pass &= run1("for_range", test_for_range(path));
            continue;
        }
        if (strcmp(name, "for_list.cfg") == 0) {
            pass &= run1("for_list", test_for_list(path));
            continue;
        }
        if (strcmp(name, "nested_for.cfg") == 0) {
            pass &= run1("nested_for", test_nested_for(path));
            continue;
        }
        if (strcmp(name, "include_main.cfg") == 0) {
            pass &= run1("include", test_include(path));
            continue;
        }
        if (strcmp(name, "dump_target.cfg") == 0) {
            if (!ran_dump) {
                pass &= run1("dump_preprocessed", test_dump_preprocessed(dir));
                ran_dump = 1;
            }
            continue;
        }
        /* error test files: run once if any error file exists */
        if (strncmp(name, "err_", 4) == 0) {
            if (!ran_errors) {
                pass &= run1("errors", test_errors(dir));
                ran_errors = 1;
            }
            continue;
        }
        /* unknown or additional test files can be added here */
    }

    closedir(dp);

    return pass ? 0 : 1;
}
