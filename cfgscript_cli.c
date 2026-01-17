/* cfgscript_cli.c - Simple CLI wrapper for cfgscript library */
#include "cfgscript.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [global options] <command> [args]\n", prog);
    fprintf(stderr, "\nGlobal options:\n");
    fprintf(stderr, "  -h, --help               - Show this help\n");
    fprintf(stderr, "  -v, --verbose            - Verbose output\n");
    fprintf(stderr, "\nCommands:\n");
    fprintf(stderr, "  load [-f|--file <file>] <file>         - Load and display config\n");
    fprintf(stderr, "  dump [-i|--in <in>] [-o|--out <out>] <in> <out> - Dump preprocessed config\n");
}

int main(int argc, char** argv) {
    int verbose = 0;

    static struct option global_opts[] = {
        {"help",    no_argument,       0, 'h'},
        {"verbose", no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };

    /* parse global options */
    optind = 1;
    while (1) {
        int opt = getopt_long(argc, argv, "hv", global_opts, NULL);
        if (opt == -1) break;
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'v':
                verbose = 1;
                break;
            case '?':
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (optind >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    const char* cmd = argv[optind++];

    if (strcmp(cmd, "load") == 0) {
        const char* file = NULL;
        static struct option load_opts[] = {
            {"file", required_argument, 0, 'f'},
            {"help", no_argument,       0, 'h'},
            {0,0,0,0}
        };

        int sub_argc = argc - optind + 1; /* include command name as argv[0] for getopt */
        char** sub_argv = argv + optind - 1;
        optind = 1;
        while (1) {
            int opt = getopt_long(sub_argc, sub_argv, "f:h", load_opts, NULL);
            if (opt == -1) break;
            switch (opt) {
                case 'f': file = optarg; break;
                case 'h': print_usage(argv[0]); return 0;
                case '?': default: print_usage(argv[0]); return 1;
            }
        }

        /* remaining positional arg may be file */
        if (!file) {
            if (optind < sub_argc) file = sub_argv[optind];
        }

        if (!file) {
            fprintf(stderr, "Usage: %s load [-f|--file <file>] <file>\n", argv[0]);
            return 1;
        }

        cfg_status_t st;
        cfg_t* cfg = cfg_load(file, &st);
        if (!cfg) {
            fprintf(stderr, "Error loading %s: %s\n", file, cfg_last_error());
            return 1;
        }

        if (verbose) printf("Loaded config from: %s\n", file);
        cfg_dump(cfg);
        cfg_free(cfg);
        return 0;
    }

    if (strcmp(cmd, "dump") == 0) {
        const char* infile = NULL;
        const char* outfile = NULL;
        static struct option dump_opts[] = {
            {"in",  required_argument, 0, 'i'},
            {"out", required_argument, 0, 'o'},
            {"help", no_argument,      0, 'h'},
            {0,0,0,0}
        };

        int sub_argc = argc - optind + 1;
        char** sub_argv = argv + optind - 1;
        optind = 1;
        while (1) {
            int opt = getopt_long(sub_argc, sub_argv, "i:o:h", dump_opts, NULL);
            if (opt == -1) break;
            switch (opt) {
                case 'i': infile = optarg; break;
                case 'o': outfile = optarg; break;
                case 'h': print_usage(argv[0]); return 0;
                case '?': default: print_usage(argv[0]); return 1;
            }
        }

        /* positional fallback */
        if (!infile) {
            if (optind < sub_argc) infile = sub_argv[optind++];
        }
        if (!outfile) {
            if (optind < sub_argc) outfile = sub_argv[optind];
        }

        if (!infile || !outfile) {
            fprintf(stderr, "Usage: %s dump [-i|--in <in>] [-o|--out <out>] <in> <out>\n", argv[0]);
            return 1;
        }

        cfg_status_t st = cfg_dump_preprocessed_file(infile, outfile, 1);
        if (st != CFG_OK) {
            fprintf(stderr, "Error: %s\n", cfg_last_error());
            return 1;
        }

        if (verbose) printf("Dumped preprocessed config from %s to %s\n", infile, outfile);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    print_usage(argv[0]);
    return 1;
}
