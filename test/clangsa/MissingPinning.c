// This file is a part of Julia. License is MIT: https://julialang.org/license

// RUN: clang -D__clang_gcanalyzer__ --analyze -Xanalyzer -analyzer-output=text -Xclang -load -Xclang libGCCheckerPlugin%shlibext -I%julia_home/src -I%julia_home/src/support -I%julia_home/usr/include ${CLANGSA_FLAGS} ${CLANGSA_CXXFLAGS} ${CPPFLAGS} ${CFLAGS} -Xclang -analyzer-checker=core,julia.GCChecker --analyzer-no-default-checks -Xclang -verify -v -x c %s

#include "julia.h"
#include "julia_internal.h"

extern void look_at_value(jl_value_t *v);

void unpinned_argument() {
    jl_svec_t *val = jl_svec1(NULL);  // expected-note{{Started tracking value here}}
    JL_GC_PROMISE_ROOTED(val);        // expected-note{{Value was rooted here}}
    look_at_value((jl_value_t*) val); // expected-warning{{Passing non-pinned value as argument to function that may GC}}
                                      // expected-note@-1{{Passing non-pinned value as argument to function that may GC}}                                      
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
