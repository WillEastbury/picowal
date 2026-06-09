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
    char      pack[32];      // optional pack prefix (e.g. "days" in "days.Name")
    char      field[32];     // field name
    query_op_t op;
    char      value[64];
} query_where_t;

// Aggregate function
typedef enum {
    QAGG_NONE = 0,  // no aggregate — group-by key (or FIRST if no aggs)
    QAGG_SUM,
    QAGG_AVG,
    QAGG_MIN,
    QAGG_MAX,
    QAGG_COUNT,
    QAGG_FIRST,
} query_agg_t;

// Parsed SELECT field
typedef struct {
    char      pack[32];      // optional pack prefix
    char      field[32];
    query_agg_t agg;         // aggregate function
} query_select_t;

// Parsed query
typedef struct {
    query_select_t select_fields[QUERY_MAX_SELECT];
    uint8_t   select_count;
    char      from_decks[4][32];   // up to 4 FROM packs
    uint8_t   from_count;
    query_where_t where[QUERY_MAX_WHERE];
    uint8_t   where_count;
    bool      valid;
} query_t;

// Parse a query string (multi-line, \n separated)
query_t query_parse(const char *text);

// Execute a parsed query. Writes pipe-delimited rows to buf.
// Sets *pack_name and *count. Returns bytes written to buf.
// Each row: field1|field2|field3\r\n
// Pipes in data escaped as \|, CR/LF escaped as \r \n
int query_execute(const query_t *q, char *buf, int buf_size,
                  const char **pack_name, int *count);

#endif
