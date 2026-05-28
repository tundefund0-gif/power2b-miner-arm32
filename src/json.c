#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "json.h"

static int json_parse_value(json_parser *p, int *i, json_token *t, int parent_idx) {
    while (isspace((unsigned char)p->text[*i])) (*i)++;
    if (*i >= (int)strlen(p->text)) return -1;

    t->start = *i;
    t->parent = parent_idx;

    switch (p->text[*i]) {
        case '"': {
            t->type = JSON_STRING;
            (*i)++;
            while (*i < (int)strlen(p->text) && p->text[*i] != '"') {
                if (p->text[*i] == '\\') (*i)++;
                (*i)++;
            }
            if (*i >= (int)strlen(p->text)) return -1;
            (*i)++;
            t->end = *i;
            break;
        }
        case '{': {
            t->type = JSON_OBJECT;
            t->size = 0;
            int tok_idx = t - p->tokens;
            (*i)++;
            while (1) {
                while (isspace((unsigned char)p->text[*i])) (*i)++;
                if (p->text[*i] == '}') { (*i)++; break; }
                if (p->num_tokens >= JSON_MAX_TOKENS) return -1;
                json_token *key = &p->tokens[p->num_tokens++];
                if (json_parse_value(p, i, key, tok_idx) < 0) return -1;
                while (isspace((unsigned char)p->text[*i])) (*i)++;
                if (p->text[*i] != ':') return -1;
                (*i)++;
                if (p->num_tokens >= JSON_MAX_TOKENS) return -1;
                json_token *val = &p->tokens[p->num_tokens++];
                if (json_parse_value(p, i, val, tok_idx) < 0) return -1;
                t->size++;
                while (isspace((unsigned char)p->text[*i])) (*i)++;
                if (p->text[*i] == ',') (*i)++;
                else if (p->text[*i] == '}') { (*i)++; break; }
                else return -1;
            }
            t->end = *i;
            break;
        }
        case '[': {
            t->type = JSON_ARRAY;
            t->size = 0;
            int tok_idx = t - p->tokens;
            (*i)++;
            while (1) {
                while (isspace((unsigned char)p->text[*i])) (*i)++;
                if (p->text[*i] == ']') { (*i)++; break; }
                if (p->num_tokens >= JSON_MAX_TOKENS) return -1;
                json_token *val = &p->tokens[p->num_tokens++];
                if (json_parse_value(p, i, val, tok_idx) < 0) return -1;
                t->size++;
                while (isspace((unsigned char)p->text[*i])) (*i)++;
                if (p->text[*i] == ',') (*i)++;
                else if (p->text[*i] == ']') { (*i)++; break; }
                else return -1;
            }
            t->end = *i;
            break;
        }
        case 't': {
            t->type = JSON_TRUE;
            if (strncmp(&p->text[*i], "true", 4) != 0) return -1;
            *i += 4;
            t->end = *i;
            break;
        }
        case 'f': {
            t->type = JSON_FALSE;
            if (strncmp(&p->text[*i], "false", 5) != 0) return -1;
            *i += 5;
            t->end = *i;
            break;
        }
        case 'n': {
            t->type = JSON_NULL;
            if (strncmp(&p->text[*i], "null", 4) != 0) return -1;
            *i += 4;
            t->end = *i;
            break;
        }
        default: {
            if (p->text[*i] == '-' || isdigit((unsigned char)p->text[*i])) {
                t->type = JSON_NUMBER;
                if (p->text[*i] == '-') (*i)++;
                while (isdigit((unsigned char)p->text[*i])) (*i)++;
                if (p->text[*i] == '.') {
                    (*i)++;
                    while (isdigit((unsigned char)p->text[*i])) (*i)++;
                }
                if (p->text[*i] == 'e' || p->text[*i] == 'E') {
                    (*i)++;
                    if (p->text[*i] == '+' || p->text[*i] == '-') (*i)++;
                    while (isdigit((unsigned char)p->text[*i])) (*i)++;
                }
                t->end = *i;
            } else {
                return -1;
            }
        }
    }
    return 0;
}

int json_parse(json_parser *p, char *text) {
    memset(p, 0, sizeof(*p));
    p->text = text;
    p->num_tokens = 1;
    int i = 0;
    return json_parse_value(p, &i, &p->tokens[0], -1);
}

json_token *json_get_key(json_parser *p, json_token *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    int obj_idx = obj - p->tokens;
    for (int i = 0; i < p->num_tokens; i++) {
        json_token *t = &p->tokens[i];
        if (t->parent != obj_idx) continue;
        if (t->type != JSON_STRING) continue;
        if (i + 1 >= p->num_tokens) return NULL;
        json_token *v = &p->tokens[i + 1];
        if (v->parent != obj_idx) continue;
        int len = t->end - t->start - 2;
        if (len > 0 && strncmp(&p->text[t->start + 1], key, len) == 0 &&
            (int)strlen(key) == len)
            return v;
    }
    return NULL;
}

json_token *json_get_index(json_parser *p, json_token *arr, int index) {
    if (!arr || arr->type != JSON_ARRAY) return NULL;
    if (index < 0 || index >= arr->size) return NULL;
    int arr_idx = arr - p->tokens;
    int count = 0;
    for (int i = 0; i < p->num_tokens; i++) {
        if (p->tokens[i].parent == arr_idx) {
            if (count == index) return &p->tokens[i];
            count++;
        }
    }
    return NULL;
}

const char *json_string_val(json_parser *p, json_token *t) {
    if (!t || t->type != JSON_STRING) return NULL;
    p->text[t->end - 1] = '\0';
    return &p->text[t->start + 1];
}

int json_int_val(json_parser *p, json_token *t) {
    if (!t || (t->type != JSON_NUMBER && t->type != JSON_TRUE && t->type != JSON_FALSE))
        return 0;
    if (t->type == JSON_TRUE) return 1;
    if (t->type == JSON_FALSE) return 0;
    p->text[t->end] = '\0';
    return atoi(&p->text[t->start]);
}

double json_double_val(json_parser *p, json_token *t) {
    if (!t || t->type != JSON_NUMBER) return 0;
    p->text[t->end] = '\0';
    return atof(&p->text[t->start]);
}

int json_array_size(json_parser *p, json_token *t) {
    if (!t || t->type != JSON_ARRAY) return 0;
    return t->size;
}

int json_object_size(json_parser *p, json_token *t) {
    if (!t || t->type != JSON_OBJECT) return 0;
    return t->size;
}
