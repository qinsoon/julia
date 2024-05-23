// This file is a part of Julia. License is MIT: https://julialang.org/license

// RUN: clang -D__clang_gcanalyzer__ --analyze -Xanalyzer -analyzer-output=text -Xclang -load -Xclang libGCCheckerPlugin%shlibext -I%julia_home/src -I%julia_home/src/support -I%julia_home/usr/include ${CLANGSA_FLAGS} ${CLANGSA_CXXFLAGS} ${CPPFLAGS} ${CFLAGS} -Xclang -analyzer-checker=core,julia.GCChecker --analyzer-no-default-checks -Xclang -verify -v -x c %s

#include "julia.h"
#include "julia_internal.h"

extern void look_at_value(jl_value_t *v);
extern void process_unrooted(jl_value_t *maybe_unrooted JL_MAYBE_UNROOTED JL_MAYBE_UNPINNED);

void unpinned_argument() {
    jl_svec_t *val = jl_svec1(NULL);  // expected-note{{Started tracking value here}}
    JL_GC_PROMISE_ROOTED(val);        // expected-note{{Value was rooted here}}
    look_at_value((jl_value_t*) val); // expected-warning{{Passing non-pinned value as argument to function that may GC}}
                                      // expected-note@-1{{Passing non-pinned value as argument to function that may GC}}                                      
}

int allow_unpinned() {
  jl_svec_t *val = jl_svec1(NULL);
  process_unrooted((jl_value_t*)val);
}

void pinned_argument() {
    jl_svec_t *val = jl_svec1(NULL);
    JL_GC_PROMISE_ROOTED(val);
    PTR_PIN(val);
    look_at_value((jl_value_t*) val);
    PTR_UNPIN(val);
}

void missing_pin_before_safepoint() {
    jl_svec_t *val = jl_svec1(NULL);    // expected-note{{Started tracking value here}}
    JL_GC_PROMISE_ROOTED(val);          // expected-note{{Value was rooted here}}
    jl_gc_safepoint();
    look_at_value((jl_value_t*) val);   // expected-warning{{Argument value may have been moved}}
                                        // expected-note@-1{{Argument value may have been moved}}
}

void pin_after_safepoint() {
    jl_svec_t *val = jl_svec1(NULL);
    JL_GC_PROMISE_ROOTED(val);
    jl_gc_safepoint();
    PTR_PIN(val); // expected-warning{{Attempt to PIN a value that is already moved}}
                  // expected-note@-1{{Attempt to PIN a value that is already moved}}
    look_at_value((jl_value_t*) val);
}

void proper_pin_before_safepoint() {
    jl_svec_t *val = jl_svec1(NULL);
    JL_GC_PROMISE_ROOTED(val);
    PTR_PIN(val);
    jl_gc_safepoint();
    look_at_value((jl_value_t*) val);
    PTR_UNPIN(val);
}

void push_tpin_value() {
    jl_svec_t *val = jl_svec1(NULL);
    JL_GC_PUSH1(&val);
    jl_gc_safepoint();
    look_at_value((jl_value_t*) val);
    JL_GC_POP();
}

void push_no_tpin_value() {
    jl_svec_t *val = jl_svec1(NULL);
    JL_GC_PUSH1_NO_TPIN(&val);
    jl_gc_safepoint();
    look_at_value((jl_value_t*) val);
    JL_GC_POP();
}

void pointer_to_pointer(jl_value_t **v) {
    // *v is not pinned.
    look_at_value(*v); // expected-warning{{Passing non-pinned value as argument to function that may GC}}
                       // expected-note@-1{{Passing non-pinned value as argument to function that may GC}}
                       // expected-note@-2{{Started tracking value here}}
}

void pointer_to_pointer2(jl_value_t* u, jl_value_t **v) {
    *v = u;
    look_at_value(*v); // expected-warning{{Passing non-pinned value as argument to function that may GC}}
                       // expected-note@-1{{Passing non-pinned value as argument to function that may GC}}
                       // expected-note@+1{{Started tracking value here (root was inherited)}}
}

extern jl_value_t *first_array_elem(jl_array_t *a JL_PROPAGATES_ROOT);

void root_propagation(jl_expr_t *expr) {
  PTR_PIN(expr->args);
  jl_value_t *val = first_array_elem(expr->args); // expected-note{{Started tracking value here}}
  PTR_UNPIN(expr->args);
  jl_gc_safepoint();
  look_at_value(val); // expected-warning{{Argument value may have been moved}}
                      // expected-note@-1{{Argument value may have been moved}}
}

void derive_ptr_alias(jl_method_instance_t *mi) {
  jl_value_t* a = mi->specTypes;
  jl_value_t* b = mi->specTypes;
  PTR_PIN(a);
  look_at_value(b);
  PTR_UNPIN(a);
}

void derive_ptr_alias2(jl_method_instance_t *mi) {
  PTR_PIN(mi->specTypes);
  look_at_value(mi->specTypes);
  PTR_UNPIN(mi->specTypes);
}

// Ignore this case for now. The checker conjures new syms for function return values.
// It pins the first return value, but cannot see the second return value is an alias of the first.
// However, we could rewrite the code so the checker can check it.
// void mtable(jl_value_t *f) {
//   PTR_PIN((jl_value_t*)jl_gf_mtable(f));
//   look_at_value((jl_value_t*)jl_gf_mtable(f));
// }

void mtable(jl_value_t *f) {
    jl_value_t* mtable = (jl_value_t*)jl_gf_mtable(f);
    PTR_PIN(mtable);
    look_at_value(mtable);
    PTR_UNPIN(mtable);
}

void pass_arg_to_non_safepoint(jl_tupletype_t *sigt) {
    jl_value_t *ati = jl_tparam(sigt, 0);
}

// Though the code loads the pointer after the safepoint, we don't know if the compiler would hoist the load before the safepoint.
// So it is fine that the checker reports this as an error.
void load_new_pointer_after_safepoint(jl_tupletype_t *t) {
    jl_value_t *a0 = jl_svecref(((jl_datatype_t*)(t))->parameters, 0);//expected-note{{Started tracking value here}}
    jl_safepoint();
    jl_value_t *a1 = jl_svecref(((jl_datatype_t*)(t))->parameters, 1);//expected-warning{{Argument value may have been moved}}
                                                                      //expected-note@-1{{Argument value may have been moved}}
}

void hoist_load_before_safepoint(jl_tupletype_t *t) {
    jl_svec_t* params = ((jl_datatype_t*)(t))->parameters; //expected-note{{Started tracking value here}}
    jl_value_t *a0 = jl_svecref(params, 0);
    jl_safepoint();
    jl_value_t *a1 = jl_svecref(params, 1); //expected-warning{{Argument value may have been moved}}
                                            //expected-note@-1{{Argument value may have been moved}}
}
