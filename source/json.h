#pragma once
// A small JSON reader (whole-document tree) and string escaping for the few messages we write.
#include <stdbool.h>

enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct JNode { int type; char *key; char *str; double num; struct JNode *child, *next; } JNode;

JNode *json_parse(const char *text);     // NULL if it isn't valid JSON
void json_free(JNode *n);
JNode *jget(const JNode *obj, const char *key);
const char *jstr(const JNode *obj, const char *key, const char *def);   // also turns numbers into text
double jnum(const JNode *obj, const char *key, double def);
bool jbool(const JNode *obj, const char *key, bool def);
bool jhas(const JNode *obj, const char *key);
int jlen(const JNode *arr);
JNode *jat(const JNode *arr, int i);
// writes s as a JSON string literal (with quotes) into out; returns length
int json_esc(char *out, int cap, const char *s);
