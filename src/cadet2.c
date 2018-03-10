//
//  cadet2.c
//  cadet
//
//  Created by Markus Rabe on 15/06/16.
//  Copyright © 2016 Saarland University. All rights reserved.
//

#include "cadet2.h"
#include "log.h"
#include "util.h"
#include "conflict_analysis.h"
#include "debug.h"
#include "certify.h"
#include "c2_validate.h"
#include "c2_traces.h"
#include "casesplits.h"
#include "skolem_dependencies.h"
#include "satsolver.h"
#include "c2_traces.h"
#include "c2_rl.h"

#include <math.h>
#include <stdint.h>
#include <sys/time.h>


C2* c2_init(Options* options) {
    C2* c2 = malloc(sizeof(C2));
    c2->qcnf = qcnf_init();
    c2->options = options;
    
    // DOMAINS
    c2->cs = casesplits_init(c2->qcnf);
    c2->skolem = skolem_init(c2->qcnf, c2->options);
    if (skolem_is_conflicted(c2->skolem)) {
        c2->state = C2_UNSAT;
    }
    c2->examples = examples_init(c2->qcnf, c2->options->examples_max_num);
    assert(!examples_is_conflicted(c2->examples));
    
    // Conflict analysis
    c2->ca = conflcit_analysis_init(c2);
    
    // Clause minimization
    c2->minimization_pa = partial_assignment_init(c2->qcnf);
    

    c2->state = C2_READY;
    c2->restarts = 0;
    c2->major_restarts = 0;
    c2->restarts_since_last_major = 0;
    c2->decisions_since_last_conflict = 0;
    c2->restart_base_decision_lvl = 0;
    c2->activity_factor = (float) 1.0;
    
    c2->current_conflict = NULL;

    // Statistics
    c2->statistics.conflicts = 0;
    c2->statistics.added_clauses = 0;
    c2->statistics.decisions = 0;
    c2->statistics.successful_conflict_clause_minimizations = 0;
    c2->statistics.learnt_clauses_total_length = 0;
    c2->statistics.cases_closed = 0;
    c2->statistics.lvls_backtracked = 0;
    c2->statistics.start_time = get_seconds();
    c2->statistics.minimization_stats = statistics_init(10000);

    c2->statistics.failed_literals_stats = statistics_init(10000);
    c2->statistics.failed_literals_conflicts = 0;

    // Magic constants
    c2->magic.initial_restart = 6; // [1..100] // depends also on restart factor
    c2->next_restart = c2->magic.initial_restart;
    c2->magic.restart_factor = (float) 1.2; // [1.01..2]
    c2->magic.conflict_var_weight = 2; // [0..5]
    c2->magic.conflict_clause_weight = 1; // [0..3]
    c2->magic.decision_var_activity_modifier = (float) 0.8; // [-3.0..2.0]
    c2->magic.decay_rate = (float) 0.99;
    c2->magic.activity_bump_value = (float) 1;
    c2->magic.major_restart_frequency = 15;
    c2->magic.replenish_frequency = 100;
    c2->next_major_restart = c2->magic.major_restart_frequency;
    c2->magic.num_restarts_before_Jeroslow_Wang = options->easy_debugging ? 1000 : 3;
    c2->magic.num_restarts_before_case_splits = options->easy_debugging ? 0 : 3;
    c2->magic.keeping_clauses_threshold = 3;

    // Magic constants for case splits
    c2->magic.skolem_success_horizon = (float) 0.9; // >0.0 && <1.0
    c2->magic.notoriousity_threshold_factor = (float) 5.0; // > 0.0 ??
    c2->magic.skolem_success_recent_average_initialization = (float) 1.0;
    c2->skolem_success_recent_average = c2->magic.skolem_success_recent_average_initialization;
    c2->case_split_depth_penalty = C2_CASE_SPLIT_DEPTH_PENALTY_LINEAR; // C2_CASE_SPLIT_DEPTH_PENALTY_QUADRATIC
    c2->conflicts_between_case_splits_countdown = 1;
    c2->magic.case_split_linear_depth_penalty_factor = options->easy_debugging ? 1 : 5;
    
    return c2;
}

void c2_free(C2* c2) {
    statistics_free(c2->statistics.failed_literals_stats);
    skolem_free(c2->skolem);
    if (c2->cs) {casesplits_free(c2->cs); c2->cs = NULL;}
    examples_free(c2->examples);
    conflict_analysis_free(c2->ca);
    qcnf_free(c2->qcnf);
    if (c2->current_conflict) {
        int_vector_free(c2->current_conflict);
    }
    partial_assignment_free(c2->minimization_pa);
    statistics_free(c2->statistics.minimization_stats);
    free(c2);
}

C2_VAR_DATA c2_initial_var_data() {
    C2_VAR_DATA vd;
    vd.activity = 0.0f;
    return vd;
}

void c2_set_activity(C2* c2, unsigned var_id, float val) {
    assert(val > -0.001);
    Var* v = var_vector_get(c2->qcnf->vars, var_id);
    if (v->var_id != 0) {
        assert(v->var_id == var_id);
        v->c2_vd.activity = val * c2->activity_factor;
    }
}

float c2_get_activity(C2* c2, unsigned var_id) {
    Var* v = var_vector_get(c2->qcnf->vars, var_id);
    assert(v->var_id == var_id);
    assert(v->c2_vd.activity > -0.001);
    assert(c2->activity_factor >= 1.0);
    return v->c2_vd.activity / c2->activity_factor;
}

void c2_increase_activity(C2* c2, unsigned var_id, float val) {
    assert(val >= 0.0);
    Var* v = var_vector_get(c2->qcnf->vars, var_id);
    assert(v->var_id == var_id);
    assert(v->c2_vd.activity > -0.001);
    assert(c2->activity_factor >= 1.0);
    v->c2_vd.activity += val * c2->activity_factor;
}

void c2_scale_activity(C2* c2, unsigned var_id, float factor) {
    assert(factor > 0.0 && factor < 1000.0); // just to be safe
    Var* v = var_vector_get(c2->qcnf->vars, var_id);
    assert(v->var_id == var_id);
    assert(v->c2_vd.activity > -0.001);
    assert(c2->activity_factor >= 1.0);
    v->c2_vd.activity *= factor;
}

void c2_rescale_activity_values(C2* c2) {
    float rescale_factor = 1.0f / c2->activity_factor;
    c2->activity_factor = 1.0f;
    for (unsigned i = 0; i < var_vector_count(c2->qcnf->vars); i++) {
        Var* v = var_vector_get(c2->qcnf->vars, i);
        if (v->var_id != 0) {
            c2_scale_activity(c2, i, rescale_factor);
        }
    }
}

// Returns NULL, if all variables are decided
Var* c2_pick_most_active_notdeterministic_variable(C2* c2) {
    Var* decision_var = NULL;
    float decision_var_activity = -1.0;
    for (unsigned i = 1; i < var_vector_count(c2->qcnf->vars); i++) {
        if (!skolem_is_deterministic(c2->skolem, i)) {
            Var* v = var_vector_get(c2->qcnf->vars, i);
            assert(!v->is_universal);
            if (v->var_id != 0) {
                assert(v->var_id == i);
                float v_activity = c2_get_activity(c2, v->var_id);
                c2_rl_print_activity(c2->options, v->var_id, v_activity);
                assert(v_activity > -0.001);
                if (decision_var_activity < v_activity) {
                    decision_var_activity = v_activity;
                    decision_var = v;
                }
            }
        }
    }
    V3("Maximal activity is %f for var %u\n", decision_var_activity, decision_var==NULL?0:decision_var->var_id);
    return decision_var;
}


void c2_backtrack_to_decision_lvl(C2 *c2, unsigned backtracking_lvl) {
    assert(backtracking_lvl <= c2->skolem->decision_lvl);
    if (backtracking_lvl == c2->skolem->decision_lvl) {
        V4("No backtracking happening.");
        return;
    }
    if (c2->state == C2_UNSAT) {
        LOG_WARNING("Backtracking from permanent conflict state. Potential inefficiency or usage mistake.\n");
    }
    V2("Backtracking to level %u\n", backtracking_lvl);
    c2->state = C2_READY;
    while (c2->skolem->decision_lvl > backtracking_lvl) {
        assert(c2->skolem->stack->push_count == c2->examples->stack->push_count);
        assert(c2->skolem->stack->push_count == c2->skolem->decision_lvl);
        skolem_pop(c2->skolem);
        examples_pop(c2->examples);
    }
}

unsigned c2_are_decisions_involved(C2* c2, Clause* conflict) {
    unsigned largest_decision_level_involved = c2->restart_base_decision_lvl;
    unsigned max_decision_lvl;
    if (c2->state == C2_SKOLEM_CONFLICT) {
        max_decision_lvl = c2->skolem->decision_lvl;
    } else {
        assert(c2->state == C2_EXAMPLES_CONFLICT);
        PartialAssignment* pa = examples_get_conflicted_assignment(c2->examples);
        max_decision_lvl = pa->decision_lvl;
    }

    for (unsigned i = 0; i < conflict->size; i++) {
        Lit lit = conflict->occs[i];
        unsigned dlvl;
        if (c2->state == C2_SKOLEM_CONFLICT) {
            dlvl = skolem_get_decision_lvl(c2->skolem,lit_to_var(lit));
        } else {
            assert(c2->state == C2_EXAMPLES_CONFLICT);
            PartialAssignment* pa = examples_get_conflicted_assignment(c2->examples);
            dlvl = partial_assignment_get_decision_lvl(pa, lit_to_var(lit));
        }

        if (dlvl > largest_decision_level_involved) {
            largest_decision_level_involved = dlvl;
        }
        assert(largest_decision_level_involved <= max_decision_lvl);
        if (largest_decision_level_involved == max_decision_lvl) {
            break;
        }
    }
    bool res = largest_decision_level_involved > c2->restart_base_decision_lvl;
    assert(! res || c2->skolem->decision_lvl > c2->restart_base_decision_lvl); // Decision involved, but no decision taken
    
    return res;
}

// Returns the second largest decision level -1 that occurs in the conflict. If no second largest decision level exists, returns 0.
unsigned c2_determine_backtracking_lvl(C2* c2, Clause* conflict) {
    int_vector* dlvls = int_vector_init();
    V2("Decision lvls in conflicted domain:");
    for (unsigned i = 0; i < conflict->size; i++) {
        Lit lit = conflict->occs[i];
        unsigned var_id = lit_to_var(lit);
        unsigned dlvl;
        if (c2->state == C2_SKOLEM_CONFLICT) {
            dlvl = skolem_get_decision_lvl(c2->skolem, var_id);
            unsigned constant_dlvl = skolem_get_dlvl_for_constant(c2->skolem, var_id);
            if (constant_dlvl < dlvl) {
                dlvl = constant_dlvl;
            }
        } else {
            assert(c2->state == C2_EXAMPLES_CONFLICT);
            PartialAssignment* pa = examples_get_conflicted_assignment(c2->examples);
            dlvl = partial_assignment_get_decision_lvl(pa, var_id);
        }
        V2(" %u", dlvl);
        int_vector_add(dlvls, (int) dlvl);
    }
    V2("\n");

    int_vector_sort(dlvls, compare_integers_natural_order);

    while (int_vector_count(dlvls) >= 2 &&
           int_vector_get(dlvls, int_vector_count(dlvls) - 1) == int_vector_get(dlvls, int_vector_count(dlvls) - 2)) {
        int_vector_remove_index(dlvls, int_vector_count(dlvls) - 1);
    }
    unsigned second_largest = 0;
    if (int_vector_count(dlvls) > 1) {
        second_largest = (unsigned) int_vector_get(dlvls, int_vector_count(dlvls) - 2);
    }
    second_largest = second_largest < c2->restart_base_decision_lvl ? c2->restart_base_decision_lvl : second_largest;
    return second_largest;
}

void c2_decay_activity(C2* c2) {
    assert(c2->activity_factor > 0);
    assert(isfinite(c2->activity_factor));
    float new_activity_factor = c2->activity_factor / c2->magic.decay_rate;
    if (isfinite(new_activity_factor) && isfinite(1.0 / c2->activity_factor) && new_activity_factor < 1000.0) {
        c2->activity_factor = new_activity_factor;
    } else {
        c2_rescale_activity_values(c2);
        c2->activity_factor *= 1 / c2->magic.decay_rate;
    }
}

float c2_Jeroslow_Wang_log_weight(vector* clauses) {
    float weight = 0;
    for (unsigned i = 1; i < vector_count(clauses); i++) {
        Clause* c = vector_get(clauses, i);
        if (c->size <= 10) {
            float power = (float) pow(2,(double) c->size);
            weight += 1.0f / power;
        }
    }
    assert(weight >= 0);
    weight += ((float)vector_count(clauses)) * 0.05f;
    return weight;
}

bool c2_is_in_conflcit(C2* c2) {
    bool res = c2->state == C2_UNSAT
            || c2->state == C2_EXAMPLES_CONFLICT
            || c2->state == C2_SKOLEM_CONFLICT;
//    assert(! res ||   c2->current_conflict); // currently not given because we eagerly clean up the current_conflict
    assert(  res || ! c2->current_conflict);
    return res;
}

void c2_propagate(C2* c2) {
    assert(c2->current_conflict == NULL);
    
    examples_propagate(c2->examples);
    if (examples_is_conflicted(c2->examples)) {
        assert(c2->state == C2_READY);
        c2->state = C2_EXAMPLES_CONFLICT;
        PartialAssignment* pa = examples_get_conflicted_assignment(c2->examples);
        c2_rl_conflict(c2->options, pa->conflicted_var);
        c2->current_conflict = analyze_assignment_conflict(c2,
                                               pa->conflicted_var,
                                               pa->conflicted_clause,
                                               pa,
                                               partial_assignment_get_value_for_conflict_analysis,
                                               partial_assignment_is_relevant_clause,
                                               partial_assignment_is_legal_dependence,
                                               partial_assignment_get_decision_lvl);
        assert(c2_is_in_conflcit(c2));
        return;
    }
    
    skolem_propagate(c2->skolem);
    if (skolem_is_conflicted(c2->skolem)) {
        c2_rl_conflict(c2->options, c2->skolem->conflict_var_id);
        assert(c2->state == C2_READY || c2->state == C2_SKOLEM_CONFLICT);
        c2->state = C2_SKOLEM_CONFLICT;
        c2->current_conflict = analyze_assignment_conflict(c2,
                                           c2->skolem->conflict_var_id,
                                           c2->skolem->conflicted_clause,
                                           c2->skolem,
                                           skolem_get_value_for_conflict_analysis,
                                           skolem_is_relevant_clause,
                                           skolem_is_legal_dependence_for_conflict_analysis,
                                           skolem_get_decision_lvl_for_conflict_analysis);
        assert(c2_is_in_conflcit(c2));
        return;
    }
}

void c2_initial_propagation(C2* c2) {
    c2_propagate(c2);
    if (! c2_is_in_conflcit(c2)) {
        // Restrict the universals to always satisfy the constraints (derived from AIGER circuits)
        for (unsigned i = 0; i < int_vector_count(c2->qcnf->universals_constraints); i++) {
            unsigned var_id = (unsigned) int_vector_get(c2->qcnf->universals_constraints, i);
            abortif( ! skolem_is_deterministic(c2->skolem, var_id), "Constraint variable is not determinsitic. This should be a constraint purely over the universals.");
            satsolver_add(c2->skolem->skolem, skolem_get_satsolver_lit(c2->skolem, (Lit) var_id));
            satsolver_clause_finished(c2->skolem->skolem);
            skolem_make_universal_assumption(c2->skolem, (Lit) var_id);
        }
        c2_propagate(c2); // initial propagation may be extended after assuming constants for constraints
    }
}

// MAIN LOOPS
void c2_run(C2* c2, unsigned remaining_conflicts) {
    
    while (remaining_conflicts > 0) {
        V4("\nEntering main loop at dlvl %u.\n", c2->skolem->decision_lvl);
        assert(c2->state == C2_READY || c2->state == C2_SKOLEM_CONFLICT || c2->state == C2_EXAMPLES_CONFLICT);
        assert(c2->skolem->decision_lvl >= c2->restart_base_decision_lvl);
        assert(c2->skolem->stack->push_count == c2->skolem->decision_lvl);
        
        c2_propagate(c2);
        
        if (c2_is_in_conflcit(c2)) {
            
            for (unsigned i = 0; i < int_vector_count(c2->current_conflict); i++) {
                int lit = int_vector_get(c2->current_conflict, i);
                qcnf_add_lit(c2->qcnf, - lit);
            }
            Clause* learnt_clause = qcnf_close_clause(c2->qcnf);
            if (learnt_clause == NULL) {
                abortif(satsolver_sat(c2->skolem->skolem) == SATSOLVER_SAT, "Conflict clause could not be created. Conflict counter: %zu", c2->statistics.conflicts);
                int_vector_free(c2->current_conflict);
                c2->current_conflict = NULL;
                c2->state = C2_CLOSE_CASE;
                return;
            }
            learnt_clause->original = false;
            c2->statistics.learnt_clauses_total_length += learnt_clause->size;
            
            if (c2->options->minimize_conflicts) {
                c2_minimize_clause(c2, learnt_clause);
            }
            
            int_vector_free(c2->current_conflict);
            c2->current_conflict = NULL;
            
            c2_print_variable_states(c2);

            remaining_conflicts -= 1;
            c2->statistics.conflicts += 1;
            if (c2->conflicts_between_case_splits_countdown > 0)
                c2->conflicts_between_case_splits_countdown--;
            
            float conflict_success_rating = (float) 1.0 / ((float) learnt_clause->size * ((float) c2->decisions_since_last_conflict) + (float) 1.0);
            c2->skolem_success_recent_average = (float) (c2->skolem_success_recent_average * c2->magic.skolem_success_horizon + conflict_success_rating * (1.0 - c2->magic.skolem_success_horizon));
            c2->decisions_since_last_conflict = 0;

            bool decisions_involved = c2_are_decisions_involved(c2, learnt_clause);
            if (decisions_involved) { // any decisions involved?
                // Update Examples database
                if (c2->skolem->state == SKOLEM_STATE_SKOLEM_CONFLICT) {
                    PartialAssignment* new_example = examples_add_assignment_from_skolem(c2->examples, c2->skolem);
                    if (new_example && partial_assignment_is_conflicted(new_example)) {
                        assert(c2->state == C2_READY);
                        c2->state = C2_EXAMPLES_CONFLICT;
                        return;
                    }
                }

                // Do CEGAR iteration(s)
                if (c2->options->cegar && c2->skolem->state == SKOLEM_STATE_SKOLEM_CONFLICT) {
                    
                    for (unsigned i = 0; i < c2->cs->cegar_magic.max_cegar_iterations_per_learnt_clause; i++) {
                        cegar_one_round_for_conflicting_assignment(c2);
                        if (c2->state == C2_UNSAT) {
                            return;
                        }
                        assert(c2->state == C2_SKOLEM_CONFLICT);
                        if (c2->cs->cegar_stats.recent_average_cube_size
                            <= c2->cs->cegar_magic.cegar_effectiveness_threshold) {
                            V4("One more round of CEGAR\n");
                            if (satsolver_sat(c2->skolem->skolem) == SATSOLVER_UNSAT) { // makes another SAT call
                                break; // simply continue; cannot conclude SAT, because check relied on assumptions in global conflict check
                            }
                        } else { // enough CEGAR
                            break;
                        }
                    }
                    assert(skolem_has_empty_domain(c2->skolem) || skolem_is_conflicted(c2->skolem));
                }
            }
            
            unsigned backtracking_lvl = c2_determine_backtracking_lvl(c2, learnt_clause);
            V2("Learnt clause has length %u. Backtracking %u lvls to lvl %u\n",
               learnt_clause->size,
               c2->skolem->decision_lvl - backtracking_lvl,
               backtracking_lvl);
            unsigned old_dlvl = c2->skolem->decision_lvl;
            c2_backtrack_to_decision_lvl(c2, backtracking_lvl);
            c2->statistics.lvls_backtracked += old_dlvl - c2->skolem->decision_lvl;
            
            c2_new_clause(c2, learnt_clause); // can bring c2->state in c2_unsat
            
            c2_decay_activity(c2);
            c2_log_clause(c2, learnt_clause);
            c2_trace_for_profiling(c2);
            c2_rl_new_clause(c2->options, learnt_clause);
#ifdef DEBUG
            c2_validate_unique_consequences(c2);
#endif
            
            assert(!skolem_is_conflicted(c2->skolem) || c2->state == C2_UNSAT);
            assert(decisions_involved || c2->options->functional_synthesis || c2->state == C2_UNSAT);
            if (c2->state == C2_UNSAT) {
                return;
            }

        } else { // No conflict
            // Now case splits and decisions are needed to make further progress.
            assert(c2->state == C2_READY);
            assert(c2->skolem->state == SKOLEM_STATE_READY);
            
            if (skolem_can_propagate(c2->skolem)) {
                continue; // can happen when a potentially conflicted variable is not actually conflicted
            }

            // try case splits
            bool progress_through_case_split = c2_casesplits_assume_single_lit(c2);
            if (c2->state == C2_SKOLEM_CONFLICT) {
                continue;
            }
            if (c2->state != C2_READY) {
                return;
            }
            if (progress_through_case_split) {
                assert(c2->conflicts_between_case_splits_countdown > 0);
                continue;
            } // Else continue picking a decision variable. Avoids runnint into a loop where case distinction is tried but nothing happens.

            assert(!skolem_can_propagate(c2->skolem));
            
            // regular decision
            Var* decision_var = NULL;
            int phase = 1;
            
            // scan for decision variable also done in RL mode, to detect SAT
            decision_var = c2_pick_most_active_notdeterministic_variable(c2);
            
            if (decision_var != NULL && c2->options->reinforcement_learning) {
                c2_rl_print_state(c2, remaining_conflicts);
                int d = c2_rl_get_decision();
                if (d == 0) {
                    c2->state = C2_ABORT_RL;
                    return;
                } else {
                    phase = d>0 ? 1 : -1;
                    decision_var = var_vector_get(c2->qcnf->vars, lit_to_var(d));
                    abortif(decision_var->is_universal, "Cannot select universal variable as decision var");
                    abortif(skolem_is_deterministic(c2->skolem, decision_var->var_id), "Cannot select deterministic variable as decision var.");
                    c2_rl_print_decision(c2->options, decision_var->var_id, phase);
                }
            }
            
            if (decision_var == NULL) { // no variable could be found; all variables have skolem functions
                c2->state = C2_CLOSE_CASE;
                return;
            } else { // take a decision
                assert(!skolem_is_conflicted(c2->skolem));
                
                if (c2->restarts >= c2->magic.num_restarts_before_Jeroslow_Wang && !c2->options->reinforcement_learning) {

                    float pos_JW_weight = c2_Jeroslow_Wang_log_weight(&decision_var->pos_occs);
                    float neg_JW_weight = c2_Jeroslow_Wang_log_weight(&decision_var->neg_occs);

                    phase = pos_JW_weight > neg_JW_weight ? 1 : -1;
                }

                c2_scale_activity(c2, decision_var->var_id, c2->magic.decision_var_activity_modifier);

                // Pushing before the actual decision is important to keep things
                // clean (think of decisions on level 0). This is not a decision yet,
                // so decision_lvl is not yet increased.
                skolem_push(c2->skolem);
                examples_push(c2->examples);

                c2->statistics.decisions += 1;
                c2->decisions_since_last_conflict += 1;
                
                // examples_decision(c2->examples, value * (Lit) decision_var_id);
                examples_decision_consistent_with_skolem(c2->examples, c2->skolem, phase * (Lit) decision_var->var_id);
                if (examples_is_conflicted(c2->examples)) {
                    V2("Examples domain is conflicted.\n");
                } else {
                    // Regular decision
                    
                    // Increase decision level, set
                    skolem_increase_decision_lvl(c2->skolem);
                    skolem_decision(c2->skolem, phase * (Lit) decision_var->var_id);
                }
            }
        }
    }
    
    abortif(c2_result(c2) != CADET_RESULT_UNKNOWN, "Expected going into restart but result known.");
    return; // results in a restart
}

cadet_res c2_result(C2* c2) {
    switch (c2->state) {
        case C2_SAT:
            assert(skolem_has_empty_domain(c2->skolem));
            return CADET_RESULT_SAT;
        case C2_UNSAT:
            assert(satsolver_state(c2->skolem->skolem) == SATSOLVER_SAT || c2->skolem->state == SKOLEM_STATE_CONSTANTS_CONLICT);
            assert(! skolem_has_empty_domain(c2->skolem));
            return CADET_RESULT_UNSAT;
        case C2_READY:
            return CADET_RESULT_UNKNOWN;
        default:
            LOG_ERROR("CAllED c2_result in state %d", c2->state);
            abort();
    }
}

cadet_res c2_check_propositional(QCNF* qcnf, Options* o) {
    V1("Using SAT solver to solve propositional problem.\n");
    SATSolver* checker = satsolver_init();
    satsolver_set_max_var(checker, (int) var_vector_count(qcnf->vars));
    for (unsigned i = 0; i < vector_count(qcnf->clauses); i++) {
        Clause* c = vector_get(qcnf->clauses, i);
        if (c) {
            for (unsigned j = 0; j < c->size; j++) {
                satsolver_add(checker, c->occs[j]);
            }
            satsolver_clause_finished(checker);
        }
    }
    sat_res res = satsolver_sat(checker);
    assert(res == SATSOLVER_SAT || res == SATSOLVER_UNSAT);
    if (o->certify_SAT && res == SATSOLVER_SAT) {
        cert_propositional_AIG_certificate_SAT(qcnf, o, checker, satsolver_deref_generic);
    }
    if (res == SATSOLVER_UNSAT) {
        int_vector* refuting_assignment = int_vector_init();
        // empty assignment
        c2_print_qdimacs_output(refuting_assignment);
        if (o->certify_UNSAT) {
            NOT_IMPLEMENTED();
        }
    }
    satsolver_free(checker);
    return res == SATSOLVER_SAT ? CADET_RESULT_SAT : CADET_RESULT_UNSAT;
}


void c2_replenish_skolem_satsolver(C2* c2) {
    V1("Replenishing satsolver\n");
    
    // To be sure we did mess up we remember the skolem data structure's decision level and stack height
    assert(c2->skolem->decision_lvl == 0);
    assert(c2->restart_base_decision_lvl == 0);
    assert(c2->skolem->decision_lvl == 0);
    
    Skolem* old_skolem = c2->skolem;
    c2->skolem = skolem_init(c2->qcnf, c2->options);
    
    Casesplits* old_cs = c2->cs;
    c2->cs = casesplits_init(c2->qcnf);
    
    c2_initial_propagation(c2); // (re-)establishes dlvl 0
    abortif(c2->state != C2_READY, "Conflicted after replenishing.");
    
    casesplits_update_interface(c2->cs, c2->skolem);
    
    assert(vector_count(old_cs->closed_cases) == 0 || c2->options->cegar || c2->options->casesplits);
    
    // Copy the cubes that we have solved already.
    casesplits_steal_cases(c2->cs, old_cs);
    
    // Replace the new interace activities by the old ones
    float_vector_free(c2->cs->interface_activities);
    c2->cs->interface_activities = old_cs->interface_activities;
    old_cs->interface_activities = NULL;
    
    c2->cs->cegar_stats = old_cs->cegar_stats;
    
    c2->cs->cegar_stats.successful_minimizations = old_cs->cegar_stats.successful_minimizations;
    c2->cs->cegar_stats.additional_assignments_num = old_cs->cegar_stats.additional_assignments_num;
    c2->cs->cegar_stats.successful_minimizations_by_additional_assignments = old_cs->cegar_stats.successful_minimizations;
    c2->cs->cegar_stats.recent_average_cube_size = old_cs->cegar_stats.recent_average_cube_size;
    
    skolem_free(old_skolem);
    casesplits_free(old_cs);
    
    abortif(c2_is_in_conflcit(c2) || c2->state != C2_READY, "Illegal state afte replenishing");
}


void c2_restart_heuristics(C2* c2) {
    c2->restarts_since_last_major += 1;
    c2->next_restart = (unsigned) (c2->next_restart * c2->magic.restart_factor) ;
    V3("Next restart in %u conflicts.\n", c2->next_restart);
    c2_rescale_activity_values(c2);
    
    if (c2->next_major_restart == c2->restarts_since_last_major) {
        c2->major_restarts += 1;
        c2->restarts_since_last_major = 0;
        c2->next_restart = c2->magic.initial_restart; // resets restart frequency
//        c2_delete_learnt_clauses_greater_than(c2, c2->magic.keeping_clauses_threshold);
        c2->magic.keeping_clauses_threshold += 1;
        V1("Major restart no %zu. Resetting all activity values to 0 and some random ones to 1.\n", c2->major_restarts);
        for (unsigned i = 0; i < var_vector_count(c2->qcnf->vars); i++) {
            if (qcnf_var_exists(c2->qcnf, i)) {
                c2_set_activity(c2, i, 0.0f);
            }
        }
        assert(c2->activity_factor == 1.0f);
        c2->next_major_restart = (size_t) (c2->next_major_restart * c2->magic.restart_factor);
    }
    
    if (c2->restarts % c2->magic.replenish_frequency == c2->magic.replenish_frequency - 1) {
        V1("Stepping out of case split.\n"); // Needed to simplify replenishing
        c2_backtrack_casesplit(c2);
//#if (USE_SOLVER == SOLVER_PICOSAT_ASSUMPTIONS)
//        c2_replenish_skolem_satsolver(c2);
//#endif
    }
}

cadet_res c2_sat(C2* c2) {

    ////// THIS RESTRICTS US TO 2QBF
    if (! qcnf_is_2QBF(c2->qcnf) && ! qcnf_is_propositional(c2->qcnf)) {
        V0("Is not 2QBF. Currently not supported.\n");
        return CADET_RESULT_UNKNOWN;
    }
    //////
    
    assert(c2->state == C2_UNSAT || c2->state == C2_SAT || c2->state == C2_READY);
    if (c2->state == C2_UNSAT || c2->state == C2_SAT) {
        goto return_result;
    }
    abortif(int_vector_count(c2->skolem->universals_assumptions) != 0, "There are universal assumptions before solving started.");
    assert(int_vector_count(c2->qcnf->universal_clauses) == 0); // they must have been detected through c2_new_clause
    
    c2_initial_propagation(c2);
    if (c2_is_in_conflcit(c2)) {
        c2->state = C2_UNSAT;
        goto return_result;
    }
    
    V1("Deterministic vars on dlvl 0: %u \n", int_vector_count(c2->skolem->determinization_order));
    if (debug_verbosity >= VERBOSITY_HIGH) {skolem_print_deterministic_vars(c2->skolem);}
    if (c2->options->miniscoping) {c2_analysis_determine_number_of_partitions(c2);}
    casesplits_update_interface(c2->cs, c2->skolem);
    if (c2->options->cegar_only) {
        cegar_solve_2QBF_by_cegar(c2, -1);
        assert(c2->state == C2_SAT || c2_is_in_conflcit(c2));
        return c2_result(c2);
    }

    while (c2->state == C2_READY) { // This loop controls the restarts
        
        c2_run(c2, c2->next_restart);
        assert(!c2_is_in_conflcit(c2) || c2->state == C2_UNSAT);
        if (c2->state == C2_CLOSE_CASE) { //} skolem_is_complete(c2->skolem) && (c2->options->casesplits || c2->options->certify_SAT)) {
            bool must_be_SAT = int_vector_count(c2->skolem->universals_assumptions) == 0; // just for safety
            c2_close_case(c2);
            assert(! must_be_SAT || c2->state == C2_SAT);
        }
        if (c2->state == C2_READY) {
            c2_backtrack_to_decision_lvl(c2, c2->restart_base_decision_lvl);
            V1("Restart %zu\n", c2->restarts);
            c2->restarts += 1;
            c2_restart_heuristics(c2);
            if (c2->options->minimize_conflicts) {c2_simplify(c2);}
        }
        
        if (c2->options->cegar_soft_conflict_limit && c2->statistics.conflicts > 1000 && ! c2->options->cegar) {
            LOG_WARNING("Switching cegar on after >1000 conflicts to save time during generation of problems for RL. Remove for normal operation.\n");
            c2->options->cegar = true;
        }
    }
return_result:
    return c2_result(c2);
}

int_vector* c2_refuting_assignment(C2* c2) {
    abortif(c2->state != C2_UNSAT, "Must be in UNSAT state.");
    int_vector* a = int_vector_init();
    
    if (satsolver_state(c2->cs->exists_solver) == SATSOLVER_UNSAT) {
        for (unsigned i = 0; i < var_vector_count(c2->qcnf->vars); i++) {
            if (qcnf_var_exists(c2->qcnf, i) && qcnf_is_universal(c2->qcnf, i) && qcnf_is_original(c2->qcnf, i)) {
                int val = cegar_get_val(c2->skolem, (int) i);
                int_vector_add(a, val * (Lit) i);
            }
        }
    } else {
        for (unsigned i = 0; i < var_vector_count(c2->qcnf->vars); i++) {
            if (qcnf_var_exists(c2->qcnf, i) && qcnf_is_universal(c2->qcnf, i) && qcnf_is_original(c2->qcnf, i)) {
                int val = skolem_get_value_for_conflict_analysis(c2->skolem, (Lit) i);
                if (val != 0) {int_vector_add(a, val * (Lit) i);}
            }
        }
    }
    return a;
}

/**
 * c2_solve_qdimacs is the traditional entry point to C2. It reads the qdimacs, then solves, then prints and checks the result after calling c2_sat.
 */
cadet_res c2_solve_qdimacs(FILE* f, Options* options) {
    C2* c2 = c2_from_file(f, options);
    if (f != stdin) {fclose(f);}

    V1("Maximal variable index: %u\n", var_vector_count(c2->qcnf->vars));
    V1("Number of clauses: %u\n", vector_count(c2->qcnf->clauses));
    V1("Number of scopes: %u\n", vector_count(c2->qcnf->scopes));

    if (qcnf_is_propositional(c2->qcnf) && ! options->use_qbf_engine_also_for_propositional_problems) {
        LOG_WARNING("Propositional problem; using SAT solver.\n");
        return c2_check_propositional(c2->qcnf, options);
    }
    
    if (options->plaisted_greenbaum_completion) {
        qcnf_plaisted_greenbaum_completion(c2->qcnf);
    }
    if (options->qbce) {
        qcnf_blocked_clause_detection(c2->qcnf);
    }

    cadet_res res = c2_sat(c2);
    if (debug_verbosity >= VERBOSITY_LOW) {
        c2_print_statistics(c2);
    }
    switch (res) {
        case CADET_RESULT_UNKNOWN:
            V0("UNKNOWN\n");
            break;
        case CADET_RESULT_SAT:
            V0("SAT\n");
            if (log_qdimacs_compliant) {
                printf("s cnf 1\n");
            }
            if (c2->options->certify_SAT) {
                cert_AIG_certificate(c2);
            }
            break;
        case CADET_RESULT_UNSAT:
            V0("UNSAT\n");
            assert(c2->state == C2_UNSAT);
            abortif(c2->options->functional_synthesis,
                    "Should not reach UNSAT output in functional synthesis mode.");
            if (log_qdimacs_compliant) {
                printf("s cnf 0\n");
            }
            if (c2->options->certify_UNSAT) {
                NOT_IMPLEMENTED();
            }
            
            V1("  UNSAT via Skolem conflict.\n");
            c2_print_qdimacs_output(c2_refuting_assignment(c2));
            abortif(c2->options->certify_internally_UNSAT && ! cert_check_UNSAT(c2),
                    "Check failed! UNSAT result could not be certified.");
            V1("Result verified.\n");

            // For conflicts from CEGAR, not sure if the code above handles this already
//            V1("  UNSAT via Cegar conflict.\n");
//            c2_print_qdimacs_output(c2->qcnf, c2->skolem, cegar_get_val);
//            abortif(c2->options->certify_internally_UNSAT
//                    && ! cert_check_UNSAT(c2->qcnf, c2->skolem, cegar_get_val),
//                    "Check failed! UNSAT result could not be certified.");
//            V1("Result verified.\n");
            
            // For conflicts from examples; not possible at the moment
//            V1("  UNSAT via Examples conflict.\n");
//            c2_print_qdimacs_output(c2->qcnf, c2->examples, examples_get_value_for_conflict_analysis);
//            abortif(c2->options->certify_internally_UNSAT
//                    && ! cert_check_UNSAT(c2->qcnf, c2->examples, examples_get_value_for_conflict_analysis) ,
//                    "Check failed! UNSAT result could not be certified.");
            break;
    }
    c2_free(c2);
    return res;
}

Clause* c2_add_lit(C2* c2, Lit lit) {
    if (lit != 0) {
        qcnf_add_lit(c2->qcnf, lit);
        return NULL;
    } else {
        Clause* c = qcnf_close_clause(c2->qcnf);
        if (c) {
            c2_new_clause(c2, c);
        }
        return c;
    }
}

void c2_new_variable(C2* c2, bool is_universal, unsigned scope_id, unsigned var_id) {
    qcnf_new_var(c2->qcnf, is_universal, scope_id, var_id);
    skolem_new_variable(c2->skolem, var_id);
}

void c2_new_clause(C2* c2, Clause* c) {
    assert(c->clause_idx <= vector_count(c2->qcnf->clauses));
    assert(c != NULL);
    examples_new_clause(c2->examples, c);
    assert(!examples_is_conflicted(c2->examples)); // need to handle this
    skolem_new_clause(c2->skolem, c);
    if (skolem_is_conflicted(c2->skolem)) {
        c2->state = C2_UNSAT;
    }
}
