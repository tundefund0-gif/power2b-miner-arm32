#ifndef JSON_H
#define JSON_H

#define JSON_MAX_TOKENS 256

typedef enum { JSON_STRING, JSON_NUMBER, JSON_OBJECT, JSON_ARRAY, JSON_TRUE, JSON_FALSE, JSON_NULL } json_type;

typedef struct {
    json_type type;
    int start;
    int end;
    int size;
} json_token;

typedef struct {
    char *text;
    json_token tokens[JSON_MAX_TOKENS];
    int num_tokens;
} json_parser;

int json_parse(json_parser *p, char *text);
json_token *json_get_key(json_parser *p, json_token *obj, const char *key);
json_token *json_get_index(json_parser *p, json_token *arr, int index);
const char *json_string_val(json_parser *p, json_token *t);
int json_int_val(json_parser *p, json_token *t);
double json_double_val(json_parser *p, json_token *t);
int json_array_size(json_parser *p, json_token *t);
int json_object_size(json_parser *p, json_token *t);

#endif
