#ifndef QUERY_H
#define QUERY_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// PicoWAL Query Engine
//
// Ultra-simple query language:
//   S:name,abbr,code          — Select fields (comma-separated)
//   F:countries               — From deck (pack name)
//   W:name|==|United Kingdom  — Where clause (ANDed)
//   W:code|IN|GB,US,DE        — Multiple W: lines = AND
//
// Operators:
//   ==  Equals
//   !=  Not Equal
//   >   Greater Than
//   <   Less Than
//   >=  Greater or Equal
//   <=  Less or Equal
//   IN  In comma-separated list
//   NI  Not In comma-separated list
//
// Example:
//   S:name,code
//   F:countries
//   W:code|IN|GB,US,DE
//
// Returns JSON array of matching cards with selected fields.
// ============================================================

// Max constraints
#define QUERY_MAX_SELECT   16
#define QUERY_MAX_WHERE    8
#define QUERY_MAX_RESULTS  100

// Operator enum
typedef enum {
    QOP_EQ = 0,   // ==
    QOP_NE,       // !=
    QOP_GT,       // >
    QOP_LT,       // <
    QOP_GE,       // >=
    QOP_LE,       // <=
    QOP_IN,       // IN
    QOP_NI,       // NI (not in)
} query_op_t;

// Parsed WHERE clause
typedef struct {
    char      field[32];
    query_op_t op;
    char      value[64];     // raw value string (or comma-sep for IN/NI)
} query_where_t;

// Parsed query
typedef struct {
    char      select_fields[QUERY_MAX_SELECT][32];
    uint8_t   select_count;
    char      from_deck[32];
    query_where_t where[QUERY_MAX_WHERE];
    uint8_t   where_count;
    bool      valid;
} query_t;

// Parse a query string (multi-line, \n separated)
query_t query_parse(const char *text);

// Execute a parsed query. Writes JSON result to buf.
// Returns bytes written.
int query_execute(const query_t *q, char *buf, int buf_size);

#endif
