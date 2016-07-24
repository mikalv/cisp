#define _CRT_SECURE_NO_WARNINGS 1
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <memory.h>
#include <ctype.h>
#ifndef _MSC_VER
# include <unistd.h>
#else
# include <io.h>
# define strdup(x) _strdup(x)
# define isatty(f) _isatty(f)
# define fileno(f) _fileno(f)
# define snprintf(b,n,f,...) _snprintf(b,n,f,__VA_ARGS__)
#endif

#ifdef _MSC_VER
# define INLINE
#else
# define INLINE inline
#endif

#define SYMBOL_CHARS "+-*/<>=&%?."

enum T {
  NODE_NIL, NODE_T, NODE_INT, NODE_DOUBLE, NODE_STRING, NODE_QUOTE, NODE_BQUOTE, NODE_IDENT,
  NODE_LAMBDA, NODE_CELL, NODE_ENV, NODE_ERROR,
};

struct _ENV;
struct _NODE;

typedef struct _NODE* (*f_do)(struct _ENV*, struct _NODE*);

typedef struct _NODE {
  int t;
  union {
    long i;
    double d;
    char* s;
    void* p;
    struct {
      struct _NODE *car;
      struct _NODE *cdr;
    };
  };
  f_do f;
  int r;
} NODE;

typedef struct {
  const char *k;
  NODE *v;
} ITEM;

typedef struct _ENV {
  int nv;
  ITEM **lv;
  int nf;
  ITEM **lf;
  int nm;
  ITEM **lm;
  struct _ENV *p;
  int r;
} ENV;

struct _SCANNER;

typedef char (*f_getc)(struct _SCANNER*);
typedef char (*f_peek)(struct _SCANNER*);
typedef char (*f_eof)(struct _SCANNER*);
typedef long (*f_pos)(struct _SCANNER*);
typedef void (*f_reset)(struct _SCANNER*);

typedef struct _SCANNER {
  void *v, *o;
  f_peek  peek;
  f_getc  getc;
  f_eof   eof;
  f_pos   pos;
  f_reset reset;
  char *err;
} SCANNER;

static NODE* parse_any(SCANNER *s);
static NODE* eval_node(ENV *env, NODE *node);
static void print_node(size_t nbuf, char *buf, NODE *node, int mode);

static NODE* new_node();
static void free_node(NODE *node);
static void free_env(ENV *env);
static NODE* do_ident_global(ENV *env, NODE *node);

static char
s_peek(SCANNER *s) {
  return s->peek(s);
}

static char
s_getc(SCANNER *s) {
  return s->getc(s);
}

static char
s_eof(SCANNER *s) {
  return s->eof(s);
}

static long
s_pos(SCANNER *s) {
  return s->pos(s);
}

static void
s_reset(SCANNER *s) {
  s->reset(s);
  if (s->err) free(s->err);
  s->err = NULL;
}

static NODE*
raise(SCANNER *s, const char *p) {
  s->err = strdup(p);
  return NULL;
}

static NODE*
invalid_token(SCANNER *s) {
  char buf[BUFSIZ];
  snprintf(buf, sizeof(buf), "invalid token at offset %ld", (long)s_pos(s));
  s->err = strdup(buf);
  return NULL;
}

static void
dump_node(NODE *node) {
  char buf[BUFSIZ];
  buf[0] = 0;
  print_node(sizeof(buf), buf, node, 1);
  puts(buf);
}

static void
skip_white(SCANNER *s) {
  while (!s_eof(s)) {
    if (s_peek(s) == ';') {
      s_getc(s);
      while (!s_eof(s) && s_peek(s) != '\n') s_getc(s);
    } else if (isspace((int)s_peek(s))) s_getc(s);
    else break;
  }
}

static NODE*
new_node() {
  NODE* node = (NODE*)malloc(sizeof(NODE));
  memset(node, 0, sizeof(NODE));
  node->t = NODE_NIL;
  node->r++;
  return node;
}

static NODE*
new_error(const char* msg) {
  NODE *node = new_node();
  node->t = NODE_ERROR;
  node->s = strdup(msg);
  return node;
}

static NODE*
new_errorn(const char* fmt, NODE *n) {
  NODE* node;
  char buf[BUFSIZ], tmp[BUFSIZ];
  tmp[0] = 0;
  print_node(sizeof(tmp), tmp, n, 0);
  snprintf(buf, sizeof(buf), fmt, tmp);
  node = new_node();
  node->t = NODE_ERROR;
  node->s = strdup(buf);
  return node;
}

static NODE*
new_errorf(const char* fmt, ...) {
  char buf[BUFSIZ];
  NODE* node;
  va_list list;
  va_start(list, fmt);
  vsnprintf(buf, sizeof(buf), fmt, list);
  va_end(list);
  node = new_node();
  node->t = NODE_ERROR;
  node->s = strdup(buf);
  return node;
}

static ENV*
new_env(ENV *p) {
  ENV* env = (ENV*)malloc(sizeof(ENV));
  memset(env, 0, sizeof(ENV));
  env->p = p;
  env->r++;
  return env;
}

static int
node_length(NODE *node) {
  int i = 0;
  if (!node) return 0;
  i++;
  while (node->cdr) {
    node = node->cdr;
    i++;
  }
  return i;
}

static int
node_narg(NODE *node) {
  int i = 0;
  if (!node) return 0;
  i++;
  while (node->cdr) {
    node = node->cdr;
    i++;
  }
  return i;
}

static INLINE int
match(const char *lhs, const char *rhs, size_t n) {
  const char *p = lhs, *e = lhs + n;
  while (p < e) if (!*rhs || *p++ != *rhs++) return 0;
  if (*rhs) return 0;
  return 1;
}

static NODE*
parse_paren(SCANNER *s) {
  NODE *head, *node, *x;

  skip_white(s);
  if (s_eof(s)) return raise(s, "unexpected end of file");

  head = node = new_node();
  node->t = NODE_CELL;
  while (!s_eof(s) && s_peek(s) != ')') {
    NODE *child = parse_any(s);
    if (child == NULL) return NULL;

    if (child->t == NODE_IDENT && !strcmp(".", child->s)) {
      if (!head->car) {
        free_node(child);
        return raise(s, "illegal dot operation");
      }
      free_node(child);

      skip_white(s);
      child = parse_any(s);
      if (child == NULL) return NULL;
      node->cdr = child;
      break;
    } else {
      if (head->car) {
        x = new_node();
        x->t = NODE_CELL;
        node->cdr = x;
        node = x;
      }
      node->car = child;
    }

    skip_white(s);
  }

  if (!head->car && !head->cdr)
    head->t = NODE_NIL;

  skip_white(s);
  return head;
}

static NODE*
parse_primitive(SCANNER *s) {
  char buf[BUFSIZ];
  size_t n = 0;
  char *e;
  NODE *c;

  c = new_node();
  while (!s_eof(s) && (isalnum(s_peek(s)) || strchr(SYMBOL_CHARS, s_peek(s)))) {
    buf[n++] = s_getc(s);
  }
  buf[n] = 0;
  if (match(buf, "nil", n)) {
    return c;
  }
  if (match(buf, "t", n)) {
    c->t = NODE_T;
    return c;
  }
  c->i = strtol(buf, &e, 10);
  if (e == buf+n) {
    c->t = NODE_INT;
    return c;
  }
  c->d = strtod(buf, &e);
  if (e == buf+n) {
    c->t = NODE_DOUBLE;
    return c;
  }
  c->t = NODE_IDENT;
  c->s = (char*)malloc(n + 1);
  memset(c->s, 0, n + 1);
  memcpy(c->s, buf, n);
  return c;
}

static NODE*
parse_quote(SCANNER *s) {
  NODE *node, *child;

  s_getc(s);
  child = parse_any(s);
  if (child == NULL) return NULL;
  node = new_node();
  node->t = NODE_QUOTE;
  node->car = child;
  return node;
}

static NODE*
parse_bquote(SCANNER *s) {
  NODE *node, *child;

  s_getc(s);
  child = parse_any(s);
  if (child == NULL) return NULL;
  node = new_node();
  node->t = NODE_BQUOTE;
  node->car = child;
  return node;
}

static NODE*
parse_string(SCANNER *s) {
  char buf[BUFSIZ];
  int n = 0;
  char c = 0;
  NODE *node;

  s_getc(s);
  while (!s_eof(s)) {
    c = s_getc(s);
    if (c == '\\' && !s_eof(s)) {
      c = s_peek(s);
      switch (c) {
      case '\\': c = '\\'; break;
      case 'b': c = '\b'; break;
      case 'f': c = '\f'; break;
      case 'n': c = '\n'; break;
      case 'r': c = '\r'; break;
      case 't': c = '\t'; break;
      default: return invalid_token(s); break;
      }
    } else if (c == '"') break;
    buf[n++] = c;
  }
  buf[n] = 0;
  if (c != '"') return invalid_token(s);

  node = new_node();
  node->t = NODE_STRING;
  node->s = strdup(buf);
  return node;
}

static NODE*
parse_any(SCANNER *s) {
  NODE *x = NULL;
  char c;

  skip_white(s);
  if (s_eof(s)) return raise(s, "unexpected end of file");

  if (s_peek(s) == '(') {
    s_getc(s);
    x = parse_paren(s);
    if (x == NULL) return NULL;
    if (!s_eof(s) && s_peek(s) == ')') {
      s_getc(s);
      return x;
    }
    return raise(s, "unexpected end of file");
  }
  c = s_peek(s);
  if (c == '\'') return parse_quote(s);
  if (c == '`') return parse_bquote(s);
  if (c == '"') return parse_string(s);
  if (isalnum((int)c) || strchr(SYMBOL_CHARS, c)) return parse_primitive(s);
  return invalid_token(s);
}

static void
print_args(size_t nbuf, char *buf, NODE *node, int mode) {
  while (node) {
    strncat(buf, " ", nbuf);
    print_node(nbuf, buf, node, mode);
    node = node->cdr;
  }
}

static void
print_cell(size_t nbuf, char *buf, NODE *node, int mode) {
  strncat(buf, "(", nbuf);

  while (node) {
    if (node->car)
      print_node(nbuf, buf, node->car, mode);
    else
      strncat(buf, "nil", nbuf);
    if (!node->cdr || node->cdr->t == NODE_NIL)
      break;
    if (node->cdr->t != NODE_CELL) {
      strncat(buf, " . ", nbuf);
      print_node(nbuf, buf, node->cdr, mode);
      break;
    }
    strncat(buf, " ", nbuf);
    node = node->cdr;
  }

  strncat(buf, ")", nbuf);
}

static void
print_str(size_t nbuf, char *buf, NODE *node, int mode) {
  char tmp[2];
  const char* p;
  if (mode) {
    strncat(buf, node->s, nbuf);
    return;
  }
  p = node->s;
  if (!p) {
    strncat(buf, "nil", nbuf);
    return;
  }
  tmp[1] = 0;
  strncat(buf, "\"", nbuf);
  while (*p) {
    switch (*p) {
    case '\\': strncat(buf, "\\\\", nbuf); p++; continue; break;
    case '\n': strncat(buf, "\\n", nbuf); p++; continue; break;
    case '\b': strncat(buf, "\\b", nbuf); p++; continue; break;
    case '\f': strncat(buf, "\\f", nbuf); p++; continue; break;
    case '\r': strncat(buf, "\\r", nbuf); p++; continue; break;
    case '\t': strncat(buf, "\\t", nbuf); p++; continue; break;
    }
    tmp[0] = *p;
    strncat(buf, tmp, nbuf);
    p++;
  }
  strncat(buf, "\"", nbuf);
}

static void
print_float(size_t nbuf, char *buf, NODE *node) {
  char tmp[BUFSIZ];
  snprintf(tmp, sizeof(tmp), "%lf", node->d);
  if (node->d == (double)(int)(node->d)) {
    char *p = tmp + strlen(tmp) - 1;
    while (p > tmp && *(p - 1) == '0') *p-- = 0;
  }
  strncat(buf, tmp, nbuf);
}

static void
print_node(size_t nbuf, char* buf, NODE *node, int mode) {
  char tmp[BUFSIZ];
  if (!node) {
    strncat(buf, "nil", nbuf);
    return;
  }
  switch (node->t) {
  case NODE_INT: snprintf(tmp, sizeof(tmp), "%ld", node->i); strncat(buf, tmp, nbuf); break;
  case NODE_DOUBLE: print_float(nbuf, buf, node); break;
  case NODE_STRING: print_str(nbuf, buf, node, mode); break;
  case NODE_IDENT: snprintf(tmp, sizeof(tmp), "%s", node->s); strncat(buf, tmp, nbuf); break;
  case NODE_NIL: strncat(buf, "nil", nbuf); break;
  case NODE_T: strncat(buf, "t", nbuf); break;
  case NODE_QUOTE: strncat(buf, "'", nbuf); print_node(nbuf, buf, node->car, mode); break;
  case NODE_CELL: print_cell(nbuf, buf, node, mode); break;
  case NODE_LAMBDA: strncat(buf, "(lambda", nbuf); print_args(nbuf, buf, node->cdr->cdr->cdr, mode); strncat(buf, ")", nbuf); break;
  default: strncat(buf, "()", nbuf); break;
  }
}

static INLINE void
free_node(NODE *node) {
  if (!node) return;
  node->r--;
  if (node->r > 0) return;
  switch (node->t) {
  case NODE_LAMBDA:
  case NODE_QUOTE:
  case NODE_CELL:
    if (node->cdr) free_node(node->cdr);
    if (node->car) free_node(node->car);
    break;
  case NODE_STRING:
  case NODE_IDENT:
  case NODE_ERROR:
  case NODE_NIL:
  case NODE_T:
    free((void*)node->s);
    break;
  case NODE_ENV:
    free_env((ENV*) node->p);
    break;
  }
  free((void*)node);
}

static void
free_env(ENV *env) {
  int i;
  env->r--;
  if (env->r > 0) return;
  for (i = 0; i < env->nv; i++) {
    free_node(env->lv[i]->v);
    free((void*)env->lv[i]->k);
    free((void*)env->lv[i]);
  }
  for (i = 0; i < env->nf; i++) {
    free_node(env->lf[i]->v);
    free((void*)env->lf[i]->k);
    free((void*)env->lf[i]);
  }
  for (i = 0; i < env->nm; i++) {
    free_node(env->lm[i]->v);
    free((void*)env->lm[i]->k);
    free((void*)env->lm[i]);
  }
  free((void*)env->lv);
  free((void*)env->lf);
  free((void*)env->lm);
  free((void*)env);
}

static int
compare_item(const void *a, const void *b) {
  return strcmp((*((ITEM**)a))->k, (*((ITEM**)b))->k);
}

static INLINE void
add_item(ITEM ***ll, int *nl, const char *k, NODE *v) {
  ITEM *ni = (ITEM*)malloc(sizeof(ITEM));
  memset(ni, 0, sizeof(ITEM));
  ni->k = strdup(k);
  ni->v = v;
  *ll = (ITEM**)realloc(*ll, sizeof(ITEM*) * (*nl + 1));
  (*ll)[*nl] = ni;
  (*nl)++;
  qsort(*ll, *nl, sizeof(ITEM*), compare_item);
}

static INLINE ITEM*
find_item(ITEM **ll, int nl, const char *k) {
  int left, right, mid, r;

  left = 0;
  right = nl - 1;
  while (left <= right) {
    mid = (left + right) / 2;
    r = strcmp(ll[mid]->k, k);
    if (r == 0) {
      return ll[mid];
    } else if (r < 0) {
      left = mid + 1;
    } else {
      right = mid - 1;
    }
  }
  return NULL;
}

static void
add_variable(ENV *env, const char *k, NODE *node) {
  ITEM *ni = find_item(env->lv, env->nv, k);
  if (ni) {
    free_node(ni->v);
    ni->v = node;
    return;
  }
  add_item(&env->lv, &env->nv, k, node);
}

static void
add_function(ENV *env, const char *k, NODE *node) {
  ITEM *ni = find_item(env->lf, env->nf, k);
  if (ni) {
    free_node(ni->v);
    ni->v = node;
    return;
  }
  add_item(&env->lf, &env->nf, k, node);
}

static void
add_macro(ENV *env, const char *k, NODE *node) {
  ITEM *ni = find_item(env->lm, env->nm, k);
  if (ni) {
    free_node(ni->v);
    ni->v = node;
    return;
  }
  add_item(&env->lm, &env->nm, k, node);
}

static long
int_value(ENV *env, NODE *node, NODE **err) {
  int r = 0;
  if (*err) return 0;
  node = eval_node(env, node);
  switch (node->t) {
  case NODE_ERROR: *err = node; return 0;
  case NODE_NIL: r = 0; break;
  case NODE_T: r = 1; break;
  case NODE_INT: r = node->i; break;
  case NODE_DOUBLE: r = (long)node->d; break;
  case NODE_QUOTE: r = int_value(env, node->car, err); break;
  default: *err = new_errorf("malformed number"); break;
  }
  free_node(node);
  return r;
}

static double
double_value(ENV *env, NODE *node, NODE **err) {
  double r = 0;
  if (*err) return 0;
  node = eval_node(env, node);
  switch (node->t) {
  case NODE_ERROR: *err = node; return 0;
  case NODE_INT: r = (double)node->i; break;
  case NODE_DOUBLE: r = node->d; break;
  case NODE_QUOTE: r = double_value(env, node->car, err); break;
  default: *err = new_errorf("malformed number"); break;
  }
  free_node(node);
  return r;
}

static NODE*
do_plus(ENV *env, NODE *alist) {
  NODE *nn, *c, *err = NULL;

  if (node_narg(alist) < 2) return new_errorn("malformed +: %s", alist);

  c = eval_node(env, alist->car);
  if (c->t == NODE_ERROR) return c;
  nn = new_node();
  nn->t = c->t;
  switch (nn->t) {
  case NODE_INT:
    nn->i = c->i;
    break;
  case NODE_DOUBLE:
    nn->d = c->d;
    break;
  case NODE_STRING:
    nn->s = strdup(c->s);
    break;
  }
  free_node(c);

  alist = alist->cdr;
  while (alist) {
    c = eval_node(env, alist->car);
    if (c->t == NODE_ERROR) {
      free_node(nn);
      return c;
    }
    switch (nn->t) {
    case NODE_INT:
      if (c->t == NODE_DOUBLE) {
        nn->d = double_value(env, nn, &err) + double_value(env, c, &err);
        nn->t = c->t;
      } else
        nn->i += int_value(env, c, &err);
      break;
    case NODE_DOUBLE:
      nn->d += double_value(env, c, &err);
      break;
    default:
      err = new_errorf("malformed number");
      break;
    }
    free_node(c);
    if (err) {
      free_node(nn);
      return err;
    }
    alist = alist->cdr;
  }
  return nn;
}

static NODE*
do_minus(ENV *env, NODE *alist) {
  NODE *nn, *c, *err = NULL;

  if (node_narg(alist) < 2) return new_errorn("malformed -: %s", alist);

  c = eval_node(env, alist->car);
  if (c->t == NODE_ERROR) return c;
  nn = new_node();
  nn->t = c->t;
  switch (nn->t) {
  case NODE_INT:
    nn->i = c->i;
    break;
  case NODE_DOUBLE:
    nn->d = c->d;
    break;
  case NODE_STRING:
    nn->s = strdup(c->s);
    break;
  }
  free_node(c);

  alist = alist->cdr;
  while (alist) {
    c = eval_node(env, alist->car);
    if (c->t == NODE_ERROR) return c;
    switch (nn->t) {
    case NODE_INT:
      if (c->t == NODE_DOUBLE) {
        nn->d = double_value(env, nn, &err) - double_value(env, c, &err);
        nn->t = c->t;
      } else
        nn->i -= int_value(env, c, &err);
      break;
    case NODE_DOUBLE:
      nn->d -= double_value(env, c, &err);
      break;
    default:
      err = new_errorf("malformed number");
      break;
    }
    free_node(c);
    if (err) {
      free_node(nn);
      return err;
    }
    alist = alist->cdr;
  }
  return nn;
}

static NODE*
do_mul(ENV *env, NODE *alist) {
  NODE *nn, *c, *err = NULL;

  if (node_narg(alist) < 2) return new_errorn("malformed *: %s", alist);

  c = eval_node(env, alist->car);
  if (c->t == NODE_ERROR) return c;
  nn = new_node();
  nn->t = c->t;
  switch (nn->t) {
  case NODE_INT:
    nn->i = c->i;
    break;
  case NODE_DOUBLE:
    nn->d = c->d;
    break;
  case NODE_STRING:
    nn->s = strdup(c->s);
    break;
  }
  free_node(c);

  alist = alist->cdr;
  while (alist) {
    c = eval_node(env, alist->car);
    if (c->t == NODE_ERROR) return c;
    switch (nn->t) {
    case NODE_INT:
      if (c->t == NODE_DOUBLE) {
        nn->d = double_value(env, nn, &err) * double_value(env, c, &err);
        nn->t = c->t;
      } else
        nn->i *= int_value(env, c, &err);
      break;
    case NODE_DOUBLE:
      nn->d *= double_value(env, c, &err);
      break;
    default:
      err = new_errorf("malformed number");
      break;
    }
    free_node(c);
    if (err) {
      free_node(nn);
      return err;
    }
    alist = alist->cdr;
  }
  return nn;
}

static NODE*
do_div(ENV *env, NODE *alist) {
  NODE *nn, *c, *err = NULL;

  if (node_narg(alist) < 2) return new_errorn("malformed /: %s", alist);

  c = eval_node(env, alist->car);
  if (c->t == NODE_ERROR) return c;
  nn = new_node();
  nn->t = c->t;
  switch (nn->t) {
  case NODE_INT:
    nn->i = c->i;
    break;
  case NODE_DOUBLE:
    nn->d = c->d;
    break;
  case NODE_STRING:
    nn->s = strdup(c->s);
    break;
  }
  free_node(c);

  alist = alist->cdr;
  while (alist) {
    c = eval_node(env, alist->car);
    if (c->t == NODE_ERROR) return c;
    switch (nn->t) {
    case NODE_INT:
      if (c->t == NODE_DOUBLE) {
        nn->d = double_value(env, nn, &err) / double_value(env, c, &err);
        nn->t = c->t;
      } else
        nn->i /= int_value(env, c, &err);
      break;
    case NODE_DOUBLE:
      nn->d /= double_value(env, c, &err);
      break;
    default:
      err = new_errorf("malformed number");
      break;
    }
    free_node(c);
    if (err) {
      free_node(nn);
      return err;
    }
    alist = alist->cdr;
  }
  return nn;
}

static NODE*
do_plus1(ENV *env, NODE *alist) {
  NODE *x, *c;

  if (node_narg(alist) != 1) return new_errorn("malformed 1+: %s", alist);

  x = eval_node(env, alist->car);
  if (x->t == NODE_ERROR) return x;

  c = new_node();
  c->t = x->t;
  switch (c->t) {
  case NODE_INT: c->i = x->i + 1; break;
  case NODE_DOUBLE: c->d = x->d + 1.0; break;
  default: free_node(c); c = new_errorf("malformed number"); break;
  }
  free_node(x);
  return c;
}

static NODE*
do_minus1(ENV *env, NODE *alist) {
  NODE *x, *c;

  if (node_narg(alist) != 1) return new_errorn("malformed 1-: %s", alist);

  x = eval_node(env, alist->car);
  if (x->t == NODE_ERROR) return x;

  c = new_node();
  c->t = x->t;
  switch (c->t) {
  case NODE_INT: c->i = x->i - 1; break;
  case NODE_DOUBLE: c->d = x->d - 1.0; break;
  default: free_node(c); c = new_errorf("malformed number"); break;
  }
  free_node(x);
  return c;
}

static NODE*
do_not(ENV *env, NODE *alist) {
  NODE *c, *err = NULL;

  if (node_narg(alist) != 1) return new_errorn("malformed not: %s", alist);

  c = new_node();
  c->t = NODE_INT;
  c->i = !int_value(env, alist->car, &err);
  if (err) {
    free_node(c);
    return err;
  }
  return c;
}

static NODE*
do_evenp(ENV *env, NODE *alist) {
  NODE *c, *err = NULL;

  if (node_narg(alist) != 1) return new_errorn("malformed evenp: %s", alist);

  c = new_node();
  if (int_value(env, alist->car, &err) % 2 == 0) {
    c->t = NODE_T;
  }
  if (err) {
    free_node(c);
    return err;
  }
  return c;
}

static NODE*
do_oddp(ENV *env, NODE *alist) {
  NODE *c, *err = NULL;

  if (node_narg(alist) != 1) return new_errorn("malformed oddp: %s", alist);

  c = new_node();
  if (int_value(env, alist->car, &err) % 2 != 0) {
    c->t = NODE_T;
  }
  if (err) {
    free_node(c);
    return err;
  }
  return c;
}

static NODE*
do_mod(ENV *env, NODE *alist) {
  NODE *c, *err = NULL;

  if (node_narg(alist) != 2) return new_errorn("malformed mod: %s", alist);

  c = new_node();
  c->t = NODE_INT;
  c->i = int_value(env, alist->car, &err) % int_value(env, alist->cdr->car, &err);
  if (err) {
    free_node(c);
    return err;
  }
  return c;
}

static NODE*
do_if(ENV *env, NODE *alist) {
  NODE *c;
  int r = 0, narg;

  narg = node_narg(alist);
  if (narg != 2 && narg != 3) return new_errorn("malformed if: %s", alist);

  c = eval_node(env, alist->car);
  if (c->t == NODE_ERROR) return c;
  switch (c->t) {
  case NODE_NIL:
    r = 0;
    break;
  case NODE_T:
    r = 1;
    break;
  case NODE_INT:
    r = c->i;
    break;
  case NODE_DOUBLE:
    r = (long)c->d;
    break;
  default:
    r = 1;
    break;
  }
  free_node(c);
  if (narg == 2) {
    if (r > 0) return eval_node(env, alist->cdr->car);
    return new_node();
  }
  return eval_node(env, r > 0 ? alist->cdr->car : alist->cdr->cdr->car);
}

static NODE*
do_gt(ENV *env, NODE *alist) {
  NODE *nn, *err = NULL;

  if (node_narg(alist) != 2) return new_errorn("malformed >: %s", alist);

  nn = new_node();
  if (double_value(env, alist->car, &err) > double_value(env, alist->cdr->car, &err)) {
    nn->t = NODE_T;
  }
  if (err) {
    free_node(nn);
    return err;
  }
  return nn;
}

static NODE*
do_ge(ENV *env, NODE *alist) {
  NODE *nn, *err = NULL;

  if (node_narg(alist) != 2) return new_errorn("malformed >=: %s", alist);

  nn = new_node();
  if (double_value(env, alist->car, &err) >= double_value(env, alist->cdr->car, &err)) {
    nn->t = NODE_T;
  }
  if (err) {
    free_node(nn);
    return err;
  }
  return nn;
}

static NODE*
do_lt(ENV *env, NODE *alist) {
  NODE *nn, *err = NULL;

  if (node_narg(alist) != 2) return new_errorn("malformed <: %s", alist);

  nn = new_node();
  if (double_value(env, alist->car, &err) < double_value(env, alist->cdr->car, &err)) {
    nn->t = NODE_T;
  }
  if (err) {
    free_node(nn);
    return err;
  }
  return nn;
}

static NODE*
do_le(ENV *env, NODE *alist) {
  NODE *nn, *err = NULL;

  if (node_narg(alist) != 2) return new_errorn("malformed <=: %s", alist);

  nn = new_node();
  if (double_value(env, alist->car, &err) <= double_value(env, alist->cdr->car, &err)) {
    nn->t = NODE_T;
  }
  if (err) {
    free_node(nn);
    return err;
  }
  return nn;
}

static NODE*
do_eq(ENV *env, NODE *alist) {
  NODE *lhs, *rhs, *nn, *err = NULL;

  if (node_narg(alist) != 2) return new_errorn("malformed =: %s", alist);

  lhs = eval_node(env, alist->car);
  rhs = eval_node(env, alist->cdr->car);
  nn = new_node();
  switch (lhs->t) {
  case NODE_INT:
    if (int_value(env, lhs, &err) == int_value(env, rhs, &err)) {
      nn->t = NODE_T;
    }
    break;
  case NODE_DOUBLE:
    if (double_value(env, lhs, &err) == double_value(env, rhs, &err)) {
      nn->t = NODE_T;
    }
    break;
  case NODE_STRING:
    if (rhs->t == NODE_STRING && !strcmp(lhs->s, rhs->s)) {
      nn->t = NODE_T;
    }
    break;
  }
  free_node(lhs);
  free_node(rhs);
  if (err) {
    free_node(nn);
    return err;
  }
  return nn;
}

static NODE*
do_print(ENV *env, NODE *alist) {
  NODE *c;
  char buf[BUFSIZ];

  if (node_narg(alist) != 1) return new_errorn("malformed print: %s", alist);

  c = eval_node(env, alist->car);
  if (c->t == NODE_ERROR) return c;
  buf[0] = 0;
  print_node(sizeof(buf), buf, c, 0);
  puts(buf);
  free_node(c);
  c = new_node();
  c->t = NODE_STRING;
  c->s = strdup(buf);
  return c;
}

static NODE*
do_println(ENV *env, NODE *alist) {
  NODE *c;
  char buf[BUFSIZ];

  if (node_narg(alist) != 1) return new_errorn("malformed println: %s", alist);

  c = eval_node(env, alist);
  if (c->t == NODE_ERROR) return c;
  buf[0] = 0;
  print_node(sizeof(buf), buf, c, 0);
  puts(buf);
  free_node(c);
  c = new_node();
  c->t = NODE_STRING;
  c->s = strdup(buf);
  return c;
}

static NODE*
do_princ(ENV *env, NODE *alist) {
  NODE *c;
  char buf[BUFSIZ];

  if (node_narg(alist) != 1) return new_errorn("malformed printc: %s", alist);

  c = eval_node(env, alist);
  if (c->t == NODE_ERROR) return c;
  buf[0] = 0;
  print_node(sizeof(buf), buf, c, 1);
  printf("%s", buf);
  free_node(c);
  c = new_node();
  c->t = NODE_STRING;
  c->s = strdup(buf);
  return c;
}

static NODE*
do_quote(ENV *env, NODE *alist) {
  if (node_narg(alist) != 1) return new_errorn("malformed quote: %s", alist);

  alist = alist->car;
  alist->r++;
  return alist;
}

static NODE*
do_let(ENV *env, NODE *alist) {
  ENV *newenv;
  NODE *x, *c;

  if (node_narg(alist) < 1) return new_errorn("malformed let: %s", alist);

  x = alist->car;
  newenv = new_env(env);

  /* TODO */
  while (x) {
    if (x->car->t != NODE_CELL) {
      free_env(newenv);
      return new_errorn("malformed let: %s", alist);
    }
    c = eval_node(env, x->car->cdr->car);
    if (x->t == NODE_ERROR) {
      free_env(newenv);
      return c;
    }
    add_variable(newenv, x->car->car->s, c);
    x = x->cdr;
  }
  alist = alist->cdr;
  c = NULL;
  while (alist) {
    if (c) free_node(c);
    c = eval_node(newenv, alist->car);
    alist = alist->cdr;
  }
  free_env(newenv);
  if (c) {
    return c;
  }
  return new_node();
}

static NODE*
do_exit(ENV *env, NODE *alist) {
  if (node_narg(alist)) return new_errorn("malformed exit: %s", alist);

  exit(0);
  return NULL;
}

static NODE*
do_setq(ENV *env, NODE *alist) {
  NODE *x, *c;

  if (node_narg(alist) < 2) return new_errorn("malformed setq: %s", alist);

  x = alist->car;
  if (x->t != NODE_IDENT) {
    return new_errorn("invalid identifier: %s", x);
  }

  c = alist->cdr->car;
  if (c->t == NODE_CELL && c->car && c->car->t == NODE_CELL) {
    c = c->car;
  }
  c = eval_node(env, c);
  if (c->t == NODE_ERROR) return c;
  while (env) {
    ITEM *ni = find_item(env->lv, env->nv, x->s);
    if (!env->p) {
      add_variable(env, x->s, c);
      break;
    }
    if (ni) {
      free_node(ni->v);
      ni->v = c;
      break;
    }
    env = env->p;
  }
  x = new_node();
  x->t = NODE_QUOTE;
  x->car = c;
  c->r++;
  return x;
}

static INLINE NODE*
do_ident(ENV *env, NODE *alist) {
  ITEM *ni;

  ni = find_item(env->lv, env->nv, alist->s);
  if (ni) {
    ni->v->r++;
    return ni->v;
  }

  if (env->p) return do_ident(env->p, alist);

  return new_errorf("unknown identity: %s", alist->s);
}

static INLINE NODE*
do_ident_global(ENV *env, NODE *node) {
  static ENV *global;
  ITEM *ni;

  if (global == NULL) {
    while (env->p) env = env->p;
    global = env;
  }

  ni = find_item(global->lv, global->nv, node->s);
  if (ni) {
    ni->v->r++;
    return ni->v;
  }

  return new_errorf("unknown identify: %s", node->s);
}

static NODE*
look_func(ENV *env, const char *k) {
  static ENV *global;
  ITEM *ni;

  if (!k) return NULL;
  if (global == NULL) {
    while (env->p) env = env->p;
    global = env;
  }

  ni = find_item(global->lf, global->nf, k);
  if (ni) {
    ni->v->r++;
    return ni->v;
  }

  return NULL;
}

static NODE*
look_macro(ENV *env, const char *k) {
  static ENV *global;
  ITEM *ni;

  if (!k) return NULL;
  if (global == NULL) {
    while (env->p) env = env->p;
    global = env;
  }

  ni = find_item(global->lm, global->nm, k);
  if (ni) {
    ni->v->r++;
    return ni->v;
  }

  return NULL;
}

static NODE*
copy_node(ENV *env, NODE *lhs) {
  ITEM *ni;
  NODE *rhs;

  if (!lhs) return NULL;
  if (lhs->t == NODE_IDENT) {
    ni = find_item(env->lv, env->nv, lhs->s);
    if (ni) {
      ni->v->r++;
      rhs = ni->v;
    } else {
      rhs = new_node();
      rhs->t = lhs->t;
      rhs->s = strdup(lhs->s);
    }
    return rhs;
  }
  rhs = new_node();
  rhs->t = lhs->t;
  switch (lhs->t) {
  case NODE_INT:
    rhs->i = lhs->i;
    break;
  case NODE_DOUBLE:
    rhs->d = lhs->d;
    break;
  case NODE_STRING:
    rhs->s = strdup(lhs->s);
    break;
  case NODE_ERROR:
    rhs->s = strdup(lhs->s);
    break;
  case NODE_LAMBDA:
  case NODE_QUOTE:
  case NODE_CELL:
    rhs->car = copy_node(env, lhs->car);
    rhs->cdr = copy_node(env, lhs->cdr);
    break;
  }
  return rhs;
}

static NODE*
call_node(ENV *env, NODE *node, NODE *alist) {
  ENV *newenv = NULL;
  NODE *x = NULL, *p = NULL, *c = NULL, *nn = NULL;
  int macro = 0;

  if (node->t == NODE_IDENT) {
    x = do_ident_global(env, node);
    if (x->f) {
      f_do f = (f_do) x->f;
      free_node(x);
      return f(env, alist);
    }
    if (x->t != NODE_LAMBDA || x->t == NODE_ERROR) {
      free_node(x);
      x = look_func(env, node->s);
      if (!x) {
        x = look_macro(env, node->s);
        if (!x) {
          return new_errorn("illegal function call: %s", node);
        }
        macro = 1;
      }
    }
    if (x->t == NODE_LAMBDA) {
      newenv = (ENV*) x->car->p;
      newenv->r++;
    }
    c = x->cdr->car;
    p = x->cdr->cdr;
  } else if (node->t == NODE_LAMBDA) {
    x = node;
    newenv = (ENV*) x->car->p;
    newenv->r++;
    c = x->cdr->car;
    p = x->cdr->cdr;
    x = node->cdr;
    x->r++;
  } else {
    return new_errorn("malformed arguments: %s", node);
  }

  if (!newenv) newenv = new_env(env);

  while (alist) {
    if (c && (c->t == NODE_IDENT || (c->car && !strcmp("&rest", c->car->s)))) {
      NODE *l = NULL, *rr = NULL, *nc;
      while (alist) {
        if (macro) {
          nn = alist->car;
          nn->r++;
        } else nn = eval_node(env, alist->car);
        if (nn->t == NODE_ERROR) {
          free_env(newenv);
          free_node(x);
          return nn;
        }

        nc = new_node();
        nc->t = NODE_CELL;
        nc->car = nn;
        if (l == NULL) {
          rr = l = nc;
        } else {
          l->cdr = nc;
          l = l->cdr;
        }
        alist = alist->cdr;
      }
      if (rr) add_variable(newenv, c->t == NODE_IDENT ? c->s : c->cdr->car->s, rr);
      break;
    }
    if (macro) {
      nn = alist->car;
      nn->r++;
    } else nn = eval_node(env, alist->car);
    if (nn->t == NODE_ERROR) {
      free_env(newenv);
      free_node(x);
      return nn;
    }
    add_variable(newenv, c->car->s, nn);
    alist = alist->cdr;
    c = c->cdr;
  }
  if (macro) {
    nn = copy_node(newenv, x->cdr->cdr->car);
    c = eval_node(newenv, nn);
    free_node(nn);
    free_env(newenv);
    free_node(x);
    return c;
  }
  c = NULL;
  while (p) {
    if (c) free_node(c);
    c = eval_node(newenv, p->car);
    if (c->t == NODE_ERROR) break;
    p = p->cdr;
  }

  free_env(newenv);
  free_node(x);
  if (c) {
    return c;
  }
  return new_node();
}

static NODE*
do_lambda(ENV *env, NODE *alist) {
  NODE *e, *x;

  if (node_narg(alist) < 2) return new_errorf("malformed lambda: %s", alist);

  e = new_node();
  e->t = NODE_ENV;
  e->p = env;
  env->r++;

  x = new_node();
  x->t = NODE_LAMBDA;
  x->car = e;
  x->cdr = alist;
  alist->r++;
  return x;
}

static NODE*
do_eval(ENV *env, NODE *alist) {
  if (node_narg(alist) != 1) return new_errorf("malformed eval: %s", alist);
  if (alist->car->t == NODE_QUOTE)
    return eval_node(env, alist->car->car);
  return eval_node(env, alist->car);
}

static NODE*
do_funcall(ENV *env, NODE *alist) {
  NODE *x;
  if (node_narg(alist) < 2) return new_errorf("malformed funcall: %s", alist);
  x = eval_node(env, alist->car);
  if (x->t == NODE_QUOTE) x = x->car;
  return call_node(env, x, alist->cdr);
}

static NODE*
do_defun(ENV *env, NODE *alist) {
  NODE *x;

  if (node_narg(alist) < 3) return new_errorn("malformed defun: %s", alist);

  x = alist->car;
  if (x->t != NODE_IDENT) {
    return new_errorn("invalid identifier: %s", x);
  }
  if (alist->cdr->car->t != NODE_CELL && alist->cdr->car->t != NODE_NIL) {
    return new_errorn("argument is not a list: %s", alist);
  }
  add_function(env, x->s, alist);
  alist->r++;
  x->r++;
  return x;
}

static NODE*
do_defmacro(ENV *env, NODE *alist) {
  NODE *x;

  if (node_narg(alist) != 3) return new_errorn("malformed defmacro: %s", alist);

  x = alist->car;
  if (x->t != NODE_IDENT) {
    return new_errorn("invalid identifier: %s", x);
  }
  add_macro(env, x->s, alist);
  alist->r++;
  x->r++;
  return x;
}

static NODE*
do_progn(ENV *env, NODE *alist) {
  NODE *c;

  c = NULL;
  while (alist) {
    if (c) free_node(c);
    c = eval_node(env, alist->car);
    if (c->t == NODE_ERROR) break;
    alist = alist->cdr;
  }
  if (c) {
    return c;
  }
  return new_node();
}

static NODE*
do_dotimes(ENV *env, NODE *alist) {
  ENV *newenv;
  NODE *c, *nn, *err = NULL;
  int i, r;

  if (node_narg(alist) != 2) return new_errorn("malformed dotimes: %s", alist);

  c = eval_node(env, alist->car->cdr->car);
  if (c->t == NODE_ERROR) return c;
  r = int_value(env, c, &err);
  free_node(c);
  if (err) return err;
  newenv = new_env(env);
  nn = new_node();
  nn->t = NODE_INT;
  add_variable(newenv, alist->car->car->s, nn);
  c = NULL;
  for (i = 0; i < r; i++) {
    nn->i = i;
    if (c) free_node(c);
    c = eval_node(newenv, alist->cdr->car);
    if (c->t == NODE_ERROR) break;
  }
  free_env(newenv);
  if (c) {
    return c;
  }
  return new_node();
}

static NODE*
do_type_of(ENV *env, NODE *alist) {
  NODE *c;
  const char *p = "unknown";

  if (!alist) return new_errorn("malformed type-of: %s", alist);

  c = eval_node(env, alist->car);
  if (c->t == NODE_ERROR) return c;
  switch (c->t) {
  case NODE_NIL: p = "null"; break;
  case NODE_T: p = "boolean"; break;
  case NODE_INT: p = "int"; break;
  case NODE_DOUBLE: p = "float"; break;
  case NODE_STRING: p = "string"; break;
  case NODE_QUOTE: p = "cons"; break;
  case NODE_CELL: p = "cons"; break;
  case NODE_LAMBDA: p = "function"; break;
  case NODE_IDENT: p = "symbol"; break;
  case NODE_ENV: p = "environment"; break;
  case NODE_ERROR: p = "error"; break;
  }
  free_node(c);
  c = new_node();
  c->t = NODE_STRING;
  c->s = strdup(p);
  return c;
}

static NODE*
do_getenv(ENV *env, NODE *alist) {
  NODE *c;
  const char *p;
  if (!alist) return new_errorn("malformed getenv: %s", alist);

  c = eval_node(env, alist->car);
  if (c->t != NODE_STRING || !c->s) {
    free_node(c);
    return new_errorn("malformed getenv: %s", alist);
  }
  p = getenv(c->s);
  free_node(c);
  if (p) {
    c = new_node();
    c->t = NODE_STRING;
    c->s = strdup(p);
    return c;
  }
  return new_node();
}

static NODE*
do_cond(ENV *env, NODE *alist) {
  NODE *c, *err = NULL;
  int r;

  if (node_narg(alist) < 1) return new_errorn("malformed cond: %s", alist);

  c = NULL;
  while (alist) {
    if (c) free_node(c);
    c = eval_node(env, alist->car);
    if (c->t == NODE_ERROR) break;
    r = int_value(env, c, &err);
    if (err) {
      free_node(c);
      return err;
    }
    if (r != 0) {
      free_node(c);
      return eval_node(env, alist->car->cdr->car);
    }
    alist = alist->cdr;
  }
  if (c) {
    return c;
  }
  return new_node();
}

static NODE*
do_car(ENV *env, NODE *alist) {
  NODE *x, *c;

  if (node_narg(alist) != 1) return new_errorn("malformed car: %s", alist);

  x = eval_node(env, alist->car);
  if (x->t == NODE_QUOTE) {
    free_node(x);
    c = new_node();
    c->t = NODE_IDENT;
    c->s = strdup("quote");
    return c;
  }
  if (x->t != NODE_CELL && x->t != NODE_NIL) {
    free_node(x);
    return new_errorn("argument is not a list: %s", alist);
  }
  if (x->car) {
    c = x->car;
    c->r++;
    free_node(x);
    return c;
  }
  free_node(x);
  return new_node();
}

static NODE*
do_cdr(ENV *env, NODE *alist) {
  NODE *x, *c;

  if (node_narg(alist) != 1) return new_errorn("malformed cdr: %s", alist);

  x = eval_node(env, alist->car);
  if (x->t == NODE_QUOTE) {
    x->t = NODE_CELL;
    return x;
  }
  if (x->t != NODE_CELL && x->t != NODE_NIL) {
    free_node(x);
    return new_errorn("argument is not a list: %s", alist);
  }

  c = x->cdr;
  if (c)
    c->r++;
  else
    c = new_node();
  free_node(x);
  return c;
}

static NODE*
do_rplaca(ENV *env, NODE *alist) {
  NODE *lhs, *rhs;

  if (node_narg(alist) != 2) return new_errorn("malformed rplaca: %s", alist);

  lhs = eval_node(env, alist->car);
  if (lhs->t == NODE_ERROR) return lhs;
  if (lhs->t != NODE_CELL || !lhs->cdr->cdr)
    return new_errorn("malformed rplaca: %s", alist);
  rhs = eval_node(env, alist->cdr->car);
  if (rhs->t == NODE_ERROR) {
    free_node(lhs);
    return rhs;
  }
  switch (lhs->car->t) {
  case NODE_INT:
    lhs->car->i = rhs->i;
    break;
  case NODE_DOUBLE:
    lhs->car->d = rhs->d;
    break;
  case NODE_STRING:
    lhs->car->s = strdup(rhs->s);
    break;
  }
  free_node(rhs);
  return lhs;
}

static NODE*
do_rplacd(ENV *env, NODE *alist) {
  NODE *lhs, *rhs;

  if (node_narg(alist) != 2) return new_errorn("malformed rplacd: %s", alist);

  lhs = eval_node(env, alist->car);
  if (lhs->t == NODE_ERROR) return lhs;
  if (lhs->t != NODE_CELL || !lhs->cdr->cdr)
    return new_errorn("malformed rplacd: %s", alist);
  rhs = eval_node(env, alist->cdr->car);
  if (rhs->t == NODE_ERROR) {
    free_node(lhs);
    return rhs;
  }
  free_node(lhs->cdr);
  lhs->cdr = rhs;
  return lhs;
}

static NODE*
do_cons(ENV *env, NODE *alist) {
  NODE *c, *lhs, *rhs;

  if (node_narg(alist) != 2) return new_errorn("malformed cons: %s", alist);

  lhs = eval_node(env, alist->car);
  if (lhs->t == NODE_ERROR) return lhs;
  rhs = eval_node(env, alist->cdr->car);
  if (rhs->t == NODE_ERROR) {
    free_node(lhs);
    return rhs;
  }
  c = new_node();
  c->t = NODE_CELL;
  c->car = lhs;
  c->cdr = rhs;
  return c;
}

static NODE*
do_length(ENV *env, NODE *alist) {
  NODE *x, *c;

  if (!alist) return new_errorn("malformed length: %s", alist);

  x = eval_node(env, alist->car);
  if (x->t != NODE_CELL && x->t != NODE_NIL && x->t != NODE_STRING) {
    free_node(x);
    return new_errorn("argument is not a list: %s", alist);
  }
  c = new_node();
  c->t = NODE_INT;
  c->i = x->t == NODE_NIL ? 0 : x->t == NODE_STRING ? (long)strlen(x->s) : node_length(x);
  free_node(x);
  return c;
}

static NODE*
do_concatenate(ENV *env, NODE *alist) {
  NODE *x, *c, *l, *nn;

  if (node_narg(alist) < 3) return new_errorn("malformed concatenate: %s", alist);

  x = eval_node(env, alist->car);
  if (x->t != NODE_IDENT) {
    free_node(x);
    return new_errorn("first argument is not a quote: %s", alist);
  }
  l = eval_node(env, alist->cdr->car);
  if (l->t != NODE_CELL && l->t != NODE_NIL && l->t != NODE_STRING) {
    free_node(x);
    return new_errorn("argument is not a list: %s", alist);
  }
  c = new_node();
  c->t = !strcmp(x->s, "string") ? NODE_STRING : NODE_CELL;

  alist = alist->cdr;
  while (alist) {
    if (c->t == NODE_STRING) {
      nn = eval_node(env, alist->car);
      if (nn->t == NODE_ERROR) {
        free_node(c);
        c = nn;
        break;
      }
      if (nn->t != NODE_STRING) {
        free_node(c);
        c = new_errorn("argument is not string: %s", nn);
        free_node(nn);
        break;
      }
      /* TODO: slow growing-up */
      if (c->s) {
        c->s = (char*)realloc(c->s, strlen(c->s) + strlen(nn->s) + 1);
        strcat(c->s, nn->s);
      } else {
        c->s = (char*)malloc(strlen(nn->s) + 1);
        strcpy(c->s, nn->s);
      }
      free_node(nn);
    }
    alist = alist->cdr;
  }
  free_node(x);
  free_node(l);
  return c;
}

static NODE*
do_make_string(ENV *env, NODE *alist) {
  NODE *x, *c;

  if (node_narg(alist) != 1) return new_errorn("malformed make-string: %s", alist);

  x = eval_node(env, alist->car);
  if (x->t == NODE_ERROR) return x;
  if (x->t != NODE_INT || x->i < 0) {
    free_node(x);
    return new_errorn("malformed make-string: %s", alist);
  }
  c = new_node();
  c->t = NODE_STRING;
  c->s = (char*)malloc(x->i + 1);
  memset(c->s, ' ', x->i);
  *(c->s + x->i) = 0;
  free_node(x);
  return c;
}

static char
file_peek(SCANNER *s) {
  int c = fgetc((FILE*)s->v);
  ungetc(c, (FILE*)s->v);
  return c;
}

static char
file_getc(SCANNER *s) {
  return fgetc((FILE*)s->v);
}

static char
file_eof(SCANNER *s) {
  return feof((FILE*)s->v);
}

static long
file_pos(SCANNER *s) {
  return ftell((FILE*)s->v);
}

static void
file_reset(SCANNER *s) {
  fseek((FILE*)s->v, 0, SEEK_SET);
}

static void
s_file_init(SCANNER *s, FILE* v) {
  s->v = (void*)v;
  s->o = (void*)v;
  s->peek  = file_peek;
  s->getc  = file_getc;
  s->eof   = file_eof;
  s->pos   = file_pos;
  s->reset = file_reset;
  s->err   = NULL;
}

#if 0
static char
string_peek(SCANNER *s) {
  return *((char*)s->v);
}

static char
string_getc(SCANNER *s) {
  char c = *((char*)s->v);
  s->v = ((char*)s->v) + 1;
  return c;
}

static char
string_eof(SCANNER *s) {
  return *((char*)s->v) == 0;
}

static long
string_pos(SCANNER *s) {
  return (long)((uintptr_t)s->v - (uintptr_t)s->o);
}

static void
string_reset(SCANNER *s) {
}

static void
s_string_init(SCANNER *s, char* v) {
  s->v = (void*)v;
  s->o = (void*)v;
  s->peek  = string_peek;
  s->getc  = string_getc;
  s->eof   = string_eof;
  s->pos   = string_pos;
  s->reset = string_reset;
  s->err   = NULL;
}
#endif

static NODE*
load_lisp(ENV *env, const char *fname) {
  NODE *ret, *top, *part;
  FILE *fp;

  fp = fopen(fname, "rb");
  if (!fp) {
    return new_errorf("%s", strerror(errno));
  }

  SCANNER sv, *s = &sv;
  s_file_init(s, fp);

  top = parse_paren(s);
  if (top == NULL) {
    NODE *e = new_node();
    e->t = NODE_ERROR;
    e->s = strdup(s->err);
    s_reset(s);
    fclose(fp);
    return e;
  }

  skip_white(s);
  if (!s_eof(s)) {
    free_node(top);
    fclose(fp);
    return raise(s, "illegal token");
  }
  fclose(fp);

  part = top;
  ret = NULL;
  while (part) {
    if (ret) free_node(ret);
    ret = eval_node(env, part);
    if (ret->t == NODE_ERROR) break;
    part = part->cdr;
  }

  free_node(top);
  return ret;
}

static NODE*
do_load(ENV *env, NODE *alist) {
  NODE *x, *ret;

  if (!alist->car) return new_errorn("malformed load: %s", alist);
  x = eval_node(env, alist->car);
  if (x->t == NODE_ERROR) return x;
  if (x->t != NODE_STRING) {
    free_node(x);
    return new_errorn("malformed load: %s", alist);
  }
  ret = load_lisp(env, x->s);
  if (ret->t == NODE_ERROR) {
    free_node(x);
    return ret;
  }
  free_node(x);
  free_node(ret);
  ret = new_node();
  ret->t = NODE_T;
  return ret;
}

static NODE*
do_apply(ENV *env, NODE *alist) {
  NODE *x, *nn;

  if (node_narg(alist) < 2) return new_errorn("malformed apply: %s", alist);

  x = eval_node(env, alist->cdr->car);
  if (x->t != NODE_CELL) {
    free_node(x);
    return new_errorn("second argument should be list: %s", alist);
  }
  if (alist->car->t == NODE_QUOTE)
    nn = call_node(env, alist->car->car, x);
  else
    nn = call_node(env, alist->car, x);
  free_node(x);
  if (nn->t == NODE_ERROR) {
    return nn;
  }
  return nn;
}

static void
add_sym(ENV *env, enum T t, const char* n, f_do f) {
  ITEM *ni;
  NODE *node;
  node = new_node();
  node->t = t;
  node->s = strdup(n);
  node->f = f;
  ni = (ITEM*)malloc(sizeof(ITEM));
  memset(ni, 0, sizeof(ITEM));
  ni->k = strdup(n);
  ni->v = node;
  env->lv = (ITEM**)realloc(env->lv, sizeof(ITEM*) * (env->nv + 1));
  env->lv[env->nv] = ni;
  env->nv++;
}

static void
add_defaults(ENV *env) {
  add_sym(env, NODE_IDENT, "%", do_mod);
  add_sym(env, NODE_IDENT, "*", do_mul);
  add_sym(env, NODE_IDENT, "+", do_plus);
  add_sym(env, NODE_IDENT, "-", do_minus);
  add_sym(env, NODE_IDENT, "/", do_div);
  add_sym(env, NODE_IDENT, "1+", do_plus1);
  add_sym(env, NODE_IDENT, "1-", do_minus1);
  add_sym(env, NODE_IDENT, "<", do_lt);
  add_sym(env, NODE_IDENT, "<=", do_le);
  add_sym(env, NODE_IDENT, "=", do_eq);
  add_sym(env, NODE_IDENT, ">", do_gt);
  add_sym(env, NODE_IDENT, ">=", do_ge);
  add_sym(env, NODE_IDENT, "apply", do_apply);
  add_sym(env, NODE_IDENT, "car", do_car);
  add_sym(env, NODE_IDENT, "cdr", do_cdr);
  add_sym(env, NODE_IDENT, "concatenate", do_concatenate);
  add_sym(env, NODE_IDENT, "cond", do_cond);
  add_sym(env, NODE_IDENT, "cons", do_cons);
  add_sym(env, NODE_IDENT, "defun", do_defun);
  add_sym(env, NODE_IDENT, "defmacro", do_defmacro);
  add_sym(env, NODE_IDENT, "dotimes", do_dotimes);
  add_sym(env, NODE_IDENT, "eq?", do_eq);
  add_sym(env, NODE_IDENT, "eval", do_eval);
  add_sym(env, NODE_IDENT, "funcall", do_funcall);
  add_sym(env, NODE_IDENT, "if", do_if);
  add_sym(env, NODE_IDENT, "lambda", do_lambda);
  add_sym(env, NODE_IDENT, "length", do_length);
  add_sym(env, NODE_IDENT, "let", do_let);
  add_sym(env, NODE_IDENT, "load", do_load);
  add_sym(env, NODE_IDENT, "make-string", do_make_string);
  add_sym(env, NODE_IDENT, "mod", do_mod);
  add_sym(env, NODE_IDENT, "not", do_not);
  add_sym(env, NODE_IDENT, "evenp", do_evenp);
  add_sym(env, NODE_IDENT, "oddp", do_oddp);
  add_sym(env, NODE_IDENT, "princ", do_princ);
  add_sym(env, NODE_IDENT, "print", do_print);
  add_sym(env, NODE_IDENT, "println", do_println);
  add_sym(env, NODE_IDENT, "progn", do_progn);
  add_sym(env, NODE_IDENT, "quote", do_quote);
  add_sym(env, NODE_IDENT, "rplaca", do_rplaca);
  add_sym(env, NODE_IDENT, "rplacd", do_rplacd);
  add_sym(env, NODE_IDENT, "setq", do_setq);
  add_sym(env, NODE_IDENT, "exit", do_exit);
  add_sym(env, NODE_IDENT, "type-of", do_type_of);
  add_sym(env, NODE_IDENT, "getenv", do_getenv);
  qsort(env->lv, env->nv, sizeof(ITEM*), compare_item);
}

static INLINE NODE*
eval_node(ENV *env, NODE *node) {
  NODE *c = NULL;
  switch (node->t) {
  case NODE_LAMBDA:
  case NODE_INT:
  case NODE_DOUBLE:
  case NODE_NIL:
  case NODE_T:
  case NODE_STRING:
  case NODE_ENV:
    node->r++;
    return node;
  case NODE_QUOTE:
    c = node->car;
    c->r++;
    return c;
  case NODE_IDENT:
    return do_ident(env, node);
  case NODE_CELL:
    c = node->car;
    if (!c) {
      return new_node();
    }
    if (c && c->t == NODE_CELL && c->car && c->car->t != NODE_LAMBDA) {
      NODE *r = eval_node(env, c);
      if (r->t == NODE_LAMBDA) {
        c = call_node(env, r, node->cdr);
        free_node(r);
        return c;
      }
      return r;
    }
    if (c->t == NODE_IDENT || c->t == NODE_LAMBDA) {
      c = call_node(env, c, node->cdr);
    }
    if (c == node->car) {
      c->r++;
      return c;
    }
    if (c) return c;
    return new_errorn("illegal function call: %s", node);
  }

  return new_error("unknown node");
}

int
main(int argc, char* argv[]) {
  ENV *env;
  NODE *node, *ret;
  SCANNER sv, *s = &sv;

  if (argc > 1) {
    int err = 0;
    env = new_env(NULL);
    add_defaults(env);
    ret = load_lisp(env, argv[1]);
    if (ret->t == NODE_ERROR) {
      fprintf(stderr, "cisp: %s\n", ret->s);
      err = 1;
    }
    free_node(ret);
    free_env(env);
    exit(err);
  }

  s_file_init(s, stdin);

  env = new_env(NULL);
  add_defaults(env);
  while (1) {
    if (isatty(fileno(stdin))) printf("> ");

    node = parse_any(s);
    if (node == NULL) {
      fprintf(stderr, "cisp: %s\n", s->err);
      s_reset(s);
      continue;
    }

    ret = eval_node(env, node);
    if (ret->t == NODE_ERROR) {
      fprintf(stderr, "cisp: %s\n", ret->s);
    } else if (isatty(fileno(stdin))) {
      dump_node(ret);
    }
    free_node(ret);
    free_node(node);
  }
  free_env(env);
  return 0;
}

/* vim:set et sw=2 cino=>2,\:0: */
