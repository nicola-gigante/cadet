#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>

#include "log.h"
#include "parse.h"
#include "cadet.h"
#include "certification.h"
#include "usage_example.h"
#include "reactive.h"
#include "options.h"
#include "util.h"
#include "cadet2.h"
#include "heap.h"

#define SEED 0
#define VERSION "2.0 beta"
void print_usage(const char* name) {
    const char* options_string =
                                "  General options:\n"
                                    "\t-v [0-4]\t\tSet the verbosity [default 0]\n"
                                    "\t-s [num]\t\tSet the seed for the random number generator\n"
                                    "\t--print \t\tPrint the qdimacs file as read.\n"
                                    "\t--no_colors \t\tSuppress colors in output.\n"
                                    "\t-c [file]\t\tWrite certificate to specified file. File ending defines Aiger formag aag/aig.\n"
                                    "\t--qbfcert\t\tWrite certificate in qbfcert-readable format. Only compatible with aag file ending.\n"
                                    "\n"
                                "  Options for CADET v2.0\n"
                                    "\t-2 \t\t\tRun CADET v2.0 (default)\n"
                                    "\t-p \t\t\tEasy debugging configuration (default off)\n"
                                    "\t--case_splits \t\tCase distinctions (default off) \n"
                                    "\t--miniscoping \t\tEnables miniscoping \n"
                                    "\t--miniscoping_info \tPrint additional info on miniscoping (default off)\n"
                                    "\t--minimize_conflicts \tConflict minimization (default off) \n"
                                    "\t--trace_learnt_clauses\tPrint (colored) learnt clauses; independent of verbosity.\n"
                                    "\t--trace_for_visualization\tPrint trace of solver states at every conflict point.\n"
                                    "\t--print_variable_names\tReplace variable numbers by names where available\n"
                                    "\t--cegar\t\t\tUse CEGAR strategy in addition to incremental determinization (default off).\n"
                                    "\t--delay_conflicts\tDelay conflict checks and instead check conflicted variables in bulk.\n"
                                    "\t--sat_by_qbf\t\tUse QBF engine also for propositional problems. Uses SAT solver by default.\n"
                                    "\t--reencode_existentials\tLift existentials to their defining quantifier level.\n"
                                    "\t--reencode3QBF\t\tParse a 3QBF instance and try to convert it to a 2QBF AIG.\n"
                                    "\t--aiger_negated\t\tNegate encoding of aiger files. Can be combined with --print.\n"
                                    "\t--aiger_controllable_inputs [string] Set prefix of controllable inputs of AIGER files (default 'pi_')\n"
                                    "\n"
                                "  Options for CADET v1.0\n"
                                    "\t-1 \t\t\tRun CADET v1.0\n"
                                    "\t-r \t\t\tReactive safety synthesis for a AIGER. Very experimental feature.\n"
                                    "\t--stats\t\t\tPrint statistics\n"
                                    ;
  printf("Usage: %s [options] file\n\n  The file can be in QDIMACS or AIGER format. Files can be compressed with gzip (ending in .gz or .gzip). \n\n%s\n", name, options_string);
}

int main(int argc, const char* argv[]) {

    // default
    Options* options = default_options();
    const char *file_name = NULL;
    long verbosity = 0;
    long seed = SEED;
    FILE* file;
    
    // scan for file name and flags
    for (int i = 1; i < argc; i++) {
        if (strlen(argv[i]) < 2) {
            LOG_ERROR("Argument '%s' is too short", argv[i]);
        }
        
        if (argv[i][0] == '-') {
            switch (argv[i][1]) {
                case 'r':
                    V0("Reactive mode, reading Aiger file.\n");
                    options->reactive = true;
                    break;
                    
                case 'c': // certification flag
                    if (i + 1 >= argc) {
                        LOG_ERROR("File name for certificate missing.\n");
                        print_usage(argv[0]);
                        return 1;
                    }
                    
                    options->certify_SAT = true;
                    options->certify_UNSAT = true;
                    options->certify_internally_UNSAT = false;
                    
                    options->certificate_file_name = argv[i+1];
                    
                    if (strcmp(options->certificate_file_name, "stdout") != 0) {
                        const char* ext = get_filename_ext(options->certificate_file_name);
                        if (! ext || strlen(ext) != 3) {
                            LOG_ERROR("Must give file extension aig or aag for certificates.\n");
                            print_usage(argv[0]);
                            return 0;
                        }
                        if (strcmp(ext, "aig") == 0) {
                            options->certificate_aiger_mode = aiger_binary_mode;
                        } else if (strcmp(ext, "aag") == 0) {
                            options->certificate_aiger_mode = aiger_ascii_mode;
                        } else {
                            LOG_ERROR("File extension of certificate must be aig or aag.\n");
                            print_usage(argv[0]);
                            return 0;
                        }
                    } else {
                        options->certificate_aiger_mode = aiger_ascii_mode;
                        log_silent = true;
                    }
                    
                    if (options->case_splits) {
                        LOG_WARNING("Case splits not compatible with certificates right now. Deactivating case splits.");
                        options->case_splits = false;
                    }
                    if (options->cadet2cegar) {
                        LOG_WARNING("CEGAR is not compatible with certificates right now. Deactivating CEGAR.");
                        options->cadet2cegar = false;
                    }
                    
                    i++;
                    break;
                    
                case 'h':
                    print_usage(argv[0]);
                    return 0;
                    
                case 'p':
                    options->easy_debugging_mode_c2 = false;
                
                case 'v':
                    // verbosity flag
                    if (i + 1 >= argc) {
                        printf("Error: Verbosity number missing\n");
                        print_usage(argv[0]);
                        return 1;
                    }
                    verbosity = strtol(argv[i+1], NULL, 0);
                    switch (verbosity) {
                        case 0:
                            debug_verbosity = VERBOSITY_NONE;
                            break;
                        case 1:
                            debug_verbosity = VERBOSITY_LOW;
                            break;
                        case 2:
                            debug_verbosity = VERBOSITY_MEDIUM;
                            break;
                        case 3:
                            debug_verbosity = VERBOSITY_HIGH;
                            break;
                        case 4:
                            debug_verbosity = VERBOSITY_ALL;
                            break;
                            
                        default:
                            printf("Error: illegal verbosity number %ld\n", verbosity);
                            print_usage(argv[0]);
                            return 1;
                    }
                    i++;
                    
                    break;
                
                case '1':
                    options->cadet_version = 1;
                    break;
                    
                case '2':
                    options->cadet_version = 2;
                    break;
                    
                case 's':
                    if (i + 1 >= argc) {
                        LOG_ERROR("Missing seed number\n");
                        print_usage(argv[0]);
                        return 1;
                    }
                    seed = strtol(argv[i+1], NULL, 0);
                    i++;
                    break;
                
                case '-': // long argument (--...)
                    if (strcmp(argv[i], "--stats") == 0) {
                        V0("Enabled printing statistics\n");
                        options->print_statistics = true;
                    } else if (strcmp(argv[i], "--disable-preprocessing") == 0) {
                        V0("Disable preprocessing\n");
                        options->preprocess = false;
                    } else if (strcmp(argv[i], "--qbfcert") == 0) {
                        options->certificate_type = QBFCERT;
                    } else if (strcmp(argv[i], "--print") == 0) {
                        options->preprocess = false;
                        options->print_qdimacs = true;
                        log_comment_prefix = true;
                        log_colors = false;
                    } else if (strcmp(argv[i], "--no_colors") == 0) {
                        log_colors = false;
                    } else if (strcmp(argv[i], "--aiger_negated") == 0) {
                        options->aiger_negated_encoding = true;
                    } else if (strcmp(argv[i], "--reencode3QBF") == 0) {
                        options->reencode3QBF = true;
                    } else if (strcmp(argv[i], "--reencode_existentials") == 0) {
                        options->reencode_existentials = ! options->reencode_existentials;
                    } else if (strcmp(argv[i], "--aiger_controllable_inputs") == 0) {
                        if (i + 1 >= argc) {
                            LOG_ERROR("Missing string for argument --aiger_controllable_inputs\n");
                            print_usage(argv[0]);
                            return 1;
                        }
                        options->aiger_controllable_inputs = argv[i+1];
                        i++;
                    } else if (strcmp(argv[i], "--case_splits") == 0) {
                        options->case_splits = ! options->case_splits;
                    } else if (strcmp(argv[i], "--minimize_conflicts") == 0) {
                        options->minimize_conflicts = ! options->minimize_conflicts;
                    } else if (strcmp(argv[i], "--miniscoping") == 0) {
                        options->miniscoping = ! options->miniscoping;
                    } else if (strcmp(argv[i], "--miniscoping_info") == 0) {
                        options->print_detailed_miniscoping_stats = ! options->print_detailed_miniscoping_stats;
                    } else if (strcmp(argv[i], "--trace_learnt_clauses") == 0) {
                        options->trace_learnt_clauses = ! options->trace_learnt_clauses;
                    } else if (strcmp(argv[i], "--trace_for_visualization") == 0) {
                        options->trace_for_visualization = true;
                        options->trace_learnt_clauses = true;
                        log_colors = false;
                    } else if (strcmp(argv[i], "--print_variable_names") == 0) {
                        options->variable_names = vector_init();
                    } else if (strcmp(argv[i], "--cegar") == 0) {
                        options->cadet2cegar = ! options->cadet2cegar;
                    } else if (strcmp(argv[i], "--sat_by_qbf") == 0) {
                        options->use_qbf_engine_also_for_propositional_problems = ! options->use_qbf_engine_also_for_propositional_problems;
                    } else if (strcmp(argv[i], "--delay_conflicts") == 0) {
                        options->delay_conflict_checks = ! options->delay_conflict_checks;
                    } else {
                        LOG_ERROR("Unknown long argument '%s'", argv[i]);
                        print_usage(argv[0]);
                        return 1;
                    }
                    break;
                
                default:
                    LOG_ERROR("Unknown argument '%s'", argv[i]);
                    print_usage(argv[0]);
                    return 1;
            }
        } else {
            // expect file name as last argument
            file_name = argv[i];
            break;
        }
    }
    
    srand((unsigned int)seed);
    
    if (options->certificate_aiger_mode == aiger_binary_mode && options->certificate_type == QBFCERT) {
        LOG_WARNING("QBFCERT cannot read aiger files in binary mode. Use .aag file extension for certificate file.\n");
    }
    if (log_comment_prefix && debug_verbosity != VERBOSITY_NONE) {
        LOG_WARNING("Verbosity is on and comment prefix is set. May result in cluttered log.");
    }
    
    if (file_name == NULL) {
        V0("Reading from stdin\n");
        file = stdin;
    } else {
        V0("Processing file \"%s\".\n", file_name);
        const char* ext = get_filename_ext(file_name);
        size_t extlen = strlen(ext);
        V4("Detected file name extension %s\n", ext);
        if ( (extlen == 2 && strcmp("gz", ext) == 0) || (extlen == 4 && strcmp("gzip", ext) == 0) ) {
            char* cmd = malloc(strlen("gzcat ") + strlen(file_name) + 5);
            sprintf(cmd, "%s '%s'", "gzcat", file_name);
            free(cmd);
            file = popen(cmd, "r");
            if (!file) {
                LOG_ERROR("Cannot open gzipped file with zcat via popen. File may not exist.\n");
                return 1;
            }
        } else {
            file = fopen(file_name, "r");
            if (!file) {
                LOG_ERROR("Cannot open file \"%s\", does not exist?\n", file_name);
                return 1;
            }
        }
    }
    
    if (options->reactive) {
        return reactive(file, options);
    }
    
    switch (options->cadet_version) {
        case 2:
            V0("CADET (version 2.0)\n");
            return c2_solve_qdimacs(file,options);
        case 1:
            V0("CADET (version 1.0)\n");
            return cadet_solve_qdimacs(file, options);
        default:
            abortif(true,"Illegal qdimacs version: %d\n",options->cadet_version);
    }
}
