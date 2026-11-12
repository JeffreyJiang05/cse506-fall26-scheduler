#ifndef MACRO_H
#define MACRO_H

/**
 * Useful macros
 */

// ------------------------------------------------------- // 

#define CAT(_0, ...) PRIMITIVE_CAT(_0, __VA_ARGS__)
#define PRIMITIVE_CAT(_0, ...) _0 ## __VA_ARGS__

// ------------------------------------------------------- // 

#define COMPLEMENT(Bool) PRIMITIVE_CAT(COMPLEMENT_, Bool)
#define COMPLEMENT_0 1
#define COMPLEMENT_1 0
#define COMPLEMENT_F T
#define COMPLEMENT_T F
#define COMPLEMENT_FALSE TRUE
#define COMPLEMENT_TRUE FALSE

// ------------------------------------------------------- // 

#define IMMEDIATE_IF(cond) PRIMITIVE_CAT(IMMEDIATE_IF_, cond)
#define IMMEDIATE_IF_0(T, ...) __VA_ARGS__
#define IMMEDIATE_IF_1(T, ...) T
#define IMMEDIATE_IF_F(T, ...) __VA_ARGS__
#define IMMEDIATE_IF_T(T, ...) T
#define IMMEDIATE_IF_FALSE(T, ...) __VA_ARGS__
#define IMMEDIATE_IF_TRUE(T, ...) T

// ------------------------------------------------------- // 

#define INC(n) PRIMITIVE_CAT(INC_, n)
#define DEC(n) PRIMITIVE_CAT(DEC_, n)
#define POW2(n) PRIMITIVE_CAT(POW_2_, n)

#include "macro_arith.h"

// ------------------------------------------------------- // 

#define VOID(...)
#define EXPAND(...) __VA_ARGS__
#define DEFER(id) id VOID()
#define OBSTRUCT(...) __VA_ARGS__ DEFER(VOID)()

#define EVAL(...)  EVAL1(EVAL1(EVAL1(__VA_ARGS__)))
#define EVAL1(...) EVAL2(EVAL2(EVAL2(__VA_ARGS__)))
#define EVAL2(...) EVAL3(EVAL3(EVAL3(__VA_ARGS__)))
#define EVAL3(...) EVAL4(EVAL4(EVAL4(__VA_ARGS__)))
#define EVAL4(...) EVAL5(EVAL5(EVAL5(__VA_ARGS__)))
#define EVAL5(...) __VA_ARGS__

// ------------------------------------------------------- // 

#define TUPLE_AT_2(x, n, ...) n 
#define CHECK(...) TUPLE_AT_2(__VA_ARGS__, 0)

#define BOOL_0 , 0
#define BOOL(x) CHECK(PRIMITIVE_CAT(BOOL_, x), 1)

#define NOT_PROBE(x) x, 1,
#define NOT_0 NOT_PROBE(~)
#define NOT(x) CHECK(PRIMITIVE_CAT(NOT_, x))

// ------------------------------------------------------- // 

#define IF(cond) IMMEDIATE_IF(BOOL(cond))
#define WHEN(cond) IF(cond)(EXPAND, VOID)

// ------------------------------------------------------- // 

#define REPEAT_HELPER(count, macro, ...) \
    WHEN(count) ( \
        OBSTRUCT(REPEAT_INDIRECT)() (DEC(count), macro, __VA_ARGS__) \
        OBSTRUCT(macro) (DEC(count), __VA_ARGS__) \
    )
#define REPEAT_INDIRECT() REPEAT_HELPER
#define REPEAT(count, macro, ...) EVAL(REPEAT_HELPER(count, macro, __VA_ARGS__))


#define WHILE_HELPER(pred, op, ...) \
    IF(pred(__VA_ARGS__)) ( \
        OBSTRUCT(WHILE_INDIRECT)() (pred, op, op(__VA_ARGS__)), \
        __VA_ARGS__ \
    )
#define WHILE_INDIRECT() WHILE_HELPER
#define WHILE(pred, op, ...) EVAL(WHILE_HELPER(pred, op, __VA_ARGS__))

// ------------------------------------------------------- // 

#define EQUAL_HELPER(...) TUPLE_AT_2(__VA_ARGS__,0)
#define EQUAL_(x, y) CHECK(EQUAL_##x##_##y)
#define EQUAL(x, y) EQUAL_(x, y)
#define NOT_EQUAL(x, y) NOT(EQUAL(x, y))

// ------------------------------------------------------- // 

#define FOR_RANGE_HELPER(begin, end, macro, ...) \
    WHEN(NOT_EQUAL(begin, end)) ( \
        OBSTRUCT(macro) (begin, __VA_ARGS__) \
        OBSTRUCT(FOR_RANGE_INDIRECT)() (INC(begin), end, macro, __VA_ARGS__) \
    )
#define FOR_RANGE_INDIRECT() FOR_RANGE_HELPER
#define FOR_RANGE(begin, end, macro, ...) EVAL(FOR_RANGE_HELPER(begin, end, macro, __VA_ARGS__))

#endif