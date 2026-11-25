#include "value.h"

const char *value_type_name(Value v) {
    switch ((ValueType)v.type) {
    case VAL_NIL: return "nil";
    case VAL_BOOL: return "bool";
    case VAL_INT: return "int";
    case VAL_STR: return "str";
    case VAL_BUF: return "buf";
    case VAL_FN: return "fn";
    case VAL_NATIVE: return "built-in fn";
    case VAL_ARR: return "array";
    case VAL_STRUCT: return "struct";
    case VAL_RING: return "ring";
    }
    return "?";
}
