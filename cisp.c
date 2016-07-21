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

#ifndef _MSC_VER
# define INLINE inline
#else
# define INLINE
#endif

#define SYMBOL_CHARS "+-*/<>=&%?."

enum T {
  NODE_NIL, NODE_T, NODE_INT, NODE_DOUBLE, NODE_STRING, NODE_QUOTE, NODE_IDENT,
  NODE_LAMBDA, NODE_CELL, NODE_ERROR,
};

typedef struct _NODE {
  int t;
  union {
    long i;
    double d;
    char* s;
  };
  struct _NODE *car;
  struct _NODE *cdr;
  void *f;
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
  struct _ENV *p;
} ENV;

typedef size_t(*f_reader)(char*, size_t, size_t, void*);

typedef struct _SCANNER {
  void *v;
  f_reader f;
} SCANNER;

typedef NODE* (*f_do)(ENV*, NODE*);

static char*
raisef(const char* msg, const char *p) {
  if (!p) return NULL;
  fprintf(stderr, "%s: %s\n", msg, p);
  return NULL;
}

static char*
raise(const char *p) {
  if (!p) return NULL;
  fprintf(stderr, "invalid token: %s\n", p);
  return NULL;
}

static const char* parse_any(NODE *node, const char *p);
static NODE* eval_node(ENV *env, NODE *node);
static void print_node(size_t nbuf, char *buf, NODE *node, int mode);
static void free_node(NODE *node);
static NODE* do_ident_global(ENV *env, NODE *node);

static void
dump_node(NODE *node) {
  char buf[BUFSIZ];
  buf[0] = 0;
  print_node(sizeof(buf), buf, node, 0);
  puts(buf);
}

static const char*
skip_white(const char *p) {
  if (!p) return NULL;
  while (*p) {
    if (*p == ';') {
      p++;
      while (*p && *p != '\n') p++;
    } else if (isspace((int)*p)) p++;
    else break;
  }
  return p;
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

static const char*
parse_paren(NODE *node, const char *p) {
  NODE *head = node, *x;
  if (!p) return NULL;
  p = skip_white(p);

  node->t = NODE_CELL;
  while (p && *p && *p != ')') {
    NODE *child = new_node();
    p = parse_any(child, p);
    if (!p) {
      free_node(child);
      return NULL;
    }

    if (child->t == NODE_IDENT && !strcmp(".", child->s)) {
      if (!head->car) {
        free_node(child);
        return raise(".");
      }
      free_node(child);

      child = new_node();

      p = skip_white(p);
      p = parse_any(child, p);
      if (!p) {
        free_node(child);
        return NULL;
      }
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

    p = skip_white(p);
  }

  if (!head->car && !head->cdr)
    head->t = NODE_NIL;

  if (p && *p) {
    p = skip_white(p);
  }
  return p;
}

static const char*
parse_ident(NODE *node, const char *p) {
  char *e;
  const char *t = p;
  while (*p && (isalnum(*p) || strchr(SYMBOL_CHARS, *p))) p++;
  if (match(t, "nil", (size_t)(p - t))) {
    node->t = NODE_NIL;
    return p;
  }
  if (match(t, "t", (size_t)(p - t))) {
    node->t = NODE_T;
    return p;
  }
  node->i = strtol(t, &e, 10);
  if (p == e) {
    node->t = NODE_INT;
    return p;
  }
  node->d = strtod(t, &e);
  if (p == e) {
    node->t = NODE_DOUBLE;
    return p;
  }
  node->t = NODE_IDENT;
  node->s = (char*)malloc((size_t)(p - t) + 1);
  memset(node->s, 0, (size_t)(p - t) + 1);
  memcpy(node->s, t, (size_t)(p - t));
  return p;
}

static const char*
parse_quote(NODE *node, const char *p) {
  NODE *child = new_node();
  p = parse_any(child, p);
  if (!p) {
    free_node(child);
    return NULL;
  }
  node->t = NODE_QUOTE;
  node->car = child;
  return p;
}

static const char*
parse_string(NODE *node, const char *p) {
  const char *t = p, *q = p;
  char *sp;
  int n = 0;
  while (*p) {
    if (*p == '\\' && *(p + 1)) p++;
    else if (*p == '"') break;
    p++;
    n++;
  }
  node->s = (char*)malloc(n + 1);
  memset(node->s, 0, n + 1);
  sp = node->s;
  while (*t) {
    if (*t == '\\' && *(t + 1)) {
      switch (*(t+1)) {
      case '\\': *sp++ = '\\'; break;
      case 'b': *sp++ = '\b'; break;
      case 'f': *sp++ = '\f'; break;
      case 'n': *sp++ = '\n'; break;
      case 'r': *sp++ = '\r'; break;
      case 't': *sp++ = '\t'; break;
      default: return raise(q); break;
      }
      t++;
      t++;
      continue;
    } else if (*t == '"') break;
    *sp++ = *t++;
  }
  *sp = 0;
  if (*t != '"') return raise(q);

  p++;
  node->t = NODE_STRING;
  return p;
}

static const char*
parse_any(NODE *node, const char *p) {
  if (!p) return NULL;
  p = skip_white(p);
  if (!*p) return p;
  if (*p == '(') {
    p = parse_paren(node, p + 1);
    if (p && *p == ')') {
      p++;
      return p;
    }
    return raisef("should be )", p);
  }
  if (*p == '\'') return parse_quote(node, p + 1);
  if (*p == '"') return parse_string(node, p + 1);
  if (isalnum(*p) || strchr(SYMBOL_CHARS, *p)) return parse_ident(node, p);
  if (*p) return raise(p);
  return p;
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
  if (node->r <= 0) {
    if (node->cdr) free_node(node->cdr);
    if (node->car) free_node(node->car);
    switch (node->t) {
    case NODE_STRING:
    case NODE_IDENT:
    case NODE_CELL:
    case NODE_LAMBDA:
    case NODE_ERROR:
    case NODE_NIL:
    case NODE_T:
      free((void*)node->s);
      break;
    }
    free((void*)node);
  }
}

static void
free_env(ENV *env) {
  int i;
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
  free((void*)env->lv);
  free((void*)env->lf);
  free((void*)env);
}

static int
compare_item(const void *a, const void *b) {
  return strcmp((*((ITEM**)a))->k, (*((ITEM**)b))->k);
}

static void
add_variable(ENV *env, const char *k, NODE *node) {
  ITEM *ni;
  int left, right, mid, r;
  ITEM **lv;

  left = 0;
  right = env->nv - 1;
  lv = env->lv;
  while (left <= right) {
    mid = (left + right) / 2;
    r = strcmp(lv[mid]->k, k);
    if (r == 0) {
      free_node(lv[mid]->v);
      lv[mid]->v = node;
      return;
    } else if (r < 0) {
      left = mid + 1;
    } else {
      right = mid - 1;
    }
  }
  ni = (ITEM*)malloc(sizeof(ITEM));
  memset(ni, 0, sizeof(ITEM));
  ni->k = strdup(k);
  ni->v = node;
  env->lv = (ITEM**)realloc(env->lv, sizeof(ITEM*) * (env->nv + 1));
  env->lv[env->nv] = ni;
  env->nv++;
  qsort(env->lv, env->nv, sizeof(ITEM*), compare_item);
}

static void
add_function(ENV *env, const char *k, NODE *node) {
  ITEM *ni;
  int left, right, mid, r;
  ITEM **lf;

  left = 0;
  right = env->nf - 1;
  lf = env->lf;
  while (left <= right) {
    mid = (left + right) / 2;
    r = strcmp(lf[mid]->k, k);
    if (r == 0) {
      free_node(lf[mid]->v);
      lf[mid]->v = node;
      return;
    } else if (r < 0) {
      left = mid + 1;
    } else {
      right = mid - 1;
    }
  }
  ni = (ITEM*)malloc(sizeof(ITEM));
  memset(ni, 0, sizeof(ITEM));
  ni->k = strdup(k);
  ni->v = node;
  env->lf = (ITEM**)realloc(env->lf, sizeof(ITEM*) * (env->nf + 1));
  env->lf[env->nf] = ni;
  env->nf++;
  qsort(env->lf, env->nf, sizeof(ITEM*), compare_item);
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
do_plus(ENV *env, NODE *node) {
  NODE *nn, *c, *err = NULL;

  if (node_narg(node) < 2) return new_errorn("malformed +: %s", node);

  node = node->cdr;
  c = eval_node(env, node->car);
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

  node = node->cdr;
  while (node) {
    c = eval_node(env, node->car);
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
    node = node->cdr;
  }
  return nn;
}

static NODE*
do_minus(ENV *env, NODE *node) {
  NODE *nn, *c, *err = NULL;

  if (node_narg(node) < 2) return new_errorn("malformed -: %s", node);

  node = node->cdr;
  c = eval_node(env, node->car);
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

  node = node->cdr;
  while (node) {
    c = eval_node(env, node->car);
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
    node = node->cdr;
  }
  return nn;
}

static NODE*
do_mul(ENV *env, NODE *node) {
  NODE *nn, *c, *err = NULL;

  if (node_narg(node) < 2) return new_errorn("malformed *: %s", node);

  node = node->cdr;
  c = eval_node(env, node->car);
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

  node = node->cdr;
  while (node) {
    c = eval_node(env, node->car);
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
    node = node->cdr;
  }
  return nn;
}

static NODE*
do_div(ENV *env, NODE *node) {
  NODE *nn, *c, *err = NULL;

  if (node_narg(node) < 2) return new_errorn("malformed /: %s", node);

  node = node->cdr;
  c = eval_node(env, node->car);
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

  node = node->cdr;
  while (node) {
    c = eval_node(env, node->car);
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
    node = node->cdr;
  }
  return nn;
}

static NODE*
do_plus1(ENV *env, NODE *node) {
  NODE *x, *c;

  if (node_narg(node) != 1) return new_errorn("malformed 1+: %s", node);

  x = eval_node(env, node->cdr->car);
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
do_minus1(ENV *env, NODE *node) {
  NODE *x, *c;

  if (node_narg(node) != 1) return new_errorn("malformed 1-: %s", node);

  x = eval_node(env, node->cdr->car);
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
do_not(ENV *env, NODE *node) {
  NODE *c, *err = NULL;

  if (node_narg(node) != 1) return new_errorn("malformed not: %s", node);

  c = new_node();
  c->t = NODE_INT;
  c->i = !int_value(env, node->cdr->car, &err);
  if (err) {
    free_node(c);
    return err;
  }
  return c;
}

static NODE*
do_evenp(ENV *env, NODE *node) {
  NODE *c, *err = NULL;

  if (node_narg(node) != 1) return new_errorn("malformed evenp: %s", node);

  c = new_node();
  if (int_value(env, node->cdr->car, &err) % 2 == 0) {
    c->t = NODE_T;
  }
  if (err) {
    free_node(c);
    return err;
  }
  return c;
}

static NODE*
do_oddp(ENV *env, NODE *node) {
  NODE *c, *err = NULL;

  if (node_narg(node) != 1) return new_errorn("malformed oddp: %s", node);

  c = new_node();
  if (int_value(env, node->cdr->car, &err) % 2 == 1) {
    c->t = NODE_T;
  }
  if (err) {
    free_node(c);
    return err;
  }
  return c;
}

static NODE*
do_mod(ENV *env, NODE *node) {
  NODE *c, *err = NULL;

  if (node_narg(node) != 2) return new_errorn("malformed mod: %s", node);

  c = new_node();
  c->t = NODE_INT;
  c->i = int_value(env, node->cdr->car, &err) % int_value(env, node->cdr->cdr->car, &err);
  if (err) {
    free_node(c);
    return err;
  }
  return c;
}

static NODE*
do_if(ENV *env, NODE *node) {
  NODE *c;
  int r = 0;

  if (node_narg(node) != 3) return new_errorn("malformed if: %s", node);

  c = eval_node(env, node->cdr->car);
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
  return eval_node(env, r > 0 ? node->cdr->cdr->car : node->cdr->cdr->cdr->car);
}

static NODE*
do_gt(ENV *env, NODE *node) {
  NODE *nn, *err = NULL;

  if (node_narg(node) != 2) return new_errorn("malformed >: %s", node);

  nn = new_node();
  if (double_value(env, node->cdr, &err) > double_value(env, node->cdr->cdr, &err)) {
    nn->t = NODE_T;
  }
  if (err) {
    free_node(nn);
    return err;
  }
  return nn;
}

static NODE*
do_ge(ENV *env, NODE *node) {
  NODE *nn, *err = NULL;

  if (node_narg(node) != 2) return new_errorn("malformed >=: %s", node);

  nn = new_node();
  if (double_value(env, node->cdr->car, &err) >= double_value(env, node->cdr->cdr->car, &err)) {
    nn->t = NODE_T;
  }
  if (err) {
    free_node(nn);
    return err;
  }
  return nn;
}

static NODE*
do_lt(ENV *env, NODE *node) {
  NODE *nn, *err = NULL;

  if (node_narg(node) != 2) return new_errorn("malformed <: %s", node);

  nn = new_node();
  if (double_value(env, node->cdr->car, &err) < double_value(env, node->cdr->cdr->car, &err)) {
    nn->t = NODE_T;
  }
  if (err) {
    free_node(nn);
    return err;
  }
  return nn;
}

static NODE*
do_le(ENV *env, NODE *node) {
  NODE *nn, *err = NULL;

  if (node_narg(node) != 2) return new_errorn("malformed <=: %s", node);

  nn = new_node();
  if (double_value(env, node->cdr->car, &err) <= double_value(env, node->cdr->cdr->car, &err)) {
    nn->t = NODE_T;
  }
  if (err) {
    free_node(nn);
    return err;
  }
  return nn;
}

static NODE*
do_eq(ENV *env, NODE *node) {
  NODE *lhs, *rhs, *nn, *err = NULL;

  if (node_narg(node) != 2) return new_errorn("malformed =: %s", node);

  lhs = eval_node(env, node->cdr->car);
  rhs = eval_node(env, node->cdr->cdr->car);
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
do_print(ENV *env, NODE *node) {
  NODE *c;
  char buf[BUFSIZ];

  if (node_narg(node) != 1) return new_errorn("malformed print: %s", node);

  c = eval_node(env, node->cdr->car);
  if (c->t == NODE_ERROR) return c;
  buf[0] = 0;
  print_node(sizeof(buf), buf, c, 0);
  puts(buf);
  return c;
}

static NODE*
do_println(ENV *env, NODE *node) {
  NODE *c;
  char buf[BUFSIZ];

  if (node_narg(node) != 1) return new_errorn("malformed println: %s", node);

  c = eval_node(env, node->cdr);
  if (c->t == NODE_ERROR) return c;
  buf[0] = 0;
  print_node(sizeof(buf), buf, c, 0);
  puts(buf);
  return c;
}

static NODE*
do_princ(ENV *env, NODE *node) {
  NODE *c;
  char buf[BUFSIZ];

  if (node_narg(node) != 1) return new_errorn("malformed printc: %s", node);

  c = eval_node(env, node->cdr);
  if (c->t == NODE_ERROR) return c;
  buf[0] = 0;
  print_node(sizeof(buf), buf, c, 1);
  printf("%s", buf);
  return c;
}

static NODE*
do_quote(ENV *env, NODE *node) {
  if (node_narg(node) != 1) return new_errorn("malformed quote: %s", node);

  if (node->cdr->t == NODE_CELL && node->cdr->car) {
    node = node->cdr->car;
    node->r++;
    return node;
  }
  node->cdr->r++;
  return node->cdr;
}

static NODE*
do_let(ENV *env, NODE *node) {
  ENV *newenv;
  NODE *x, *c;

  if (node_narg(node) != 2) return new_errorn("malformed let: %s", node);

  x = node->cdr->car;
  newenv = new_env(env);

  /* TODO */
  while (x) {
    if (x->car->t != NODE_CELL) {
      return new_errorn("malformed let: %s", node);
    }
    c = eval_node(env, x->car->cdr->car);
    if (x->t == NODE_ERROR) return c;
    add_variable(newenv, x->car->car->s, c);
    x = x->cdr;
  }
  node = node->cdr->cdr;
  c = NULL;
  while (node) {
    if (c) free_node(c);
    c = eval_node(newenv, node->car);
    node = node->cdr;
  }
  free_env(newenv);
  if (c) {
    return c;
  }
  return new_node();
}

static NODE*
do_exit(ENV *env, NODE *node) {
  if (node_narg(node)) return new_errorn("malformed exit: %s", node);

  exit(0);
  return NULL;
}

static NODE*
do_setq(ENV *env, NODE *node) {
  NODE *x, *c;
  static ENV *global;

  if (node_narg(node) < 2) return new_errorn("malformed setq: %s", node);

  x = node->cdr->car;
  if (x->t != NODE_IDENT) {
    return new_errorn("invalid identifier: %s", x);
  }

  if (global == NULL) {
    while (env->p) env = env->p;
    global = env;
  }

  c = node->cdr->cdr->car;
  if (c->t == NODE_CELL && c->car && c->car->t == NODE_CELL) {
    c = c->car;
  }
  c = eval_node(env, c);
  if (c->t == NODE_ERROR) return c;
  add_variable(global, x->s, c);
  c->r++;
  return c;
}

static INLINE NODE*
do_ident(ENV *env, NODE *node) {
  NODE *x;
  const char *p = node->s;
  int left, right, mid, r;
  ITEM **lv;

  left = 0;
  right = env->nv - 1;
  lv = env->lv;
  while (left <= right) {
    mid = (left + right) / 2;
    r = strcmp(lv[mid]->k, p);
    if (r == 0) {
      x = lv[mid]->v;
      x->r++;
      return x;
    } else if (r < 0) {
      left = mid + 1;
    } else {
      right = mid - 1;
    }
  }

  while (env->p) env = env->p;

  left = 0;
  right = env->nv - 1;
  lv = env->lv;
  while (left <= right) {
    mid = (left + right) / 2;
    r = strcmp(lv[mid]->k, p);
    if (r == 0) {
      x = lv[mid]->v;
      x->r++;
      return x;
    } else if (r < 0) {
      left = mid + 1;
    } else {
      right = mid - 1;
    }
  }

  return new_errorf("unknown variable: %s", node->s);
}

static INLINE NODE*
do_ident_global(ENV *env, NODE *node) {
  NODE *x;
  static ENV *global;
  const char *p = node->s;
  int left, right, mid, r;
  ITEM **lv;

  if (global == NULL) {
    while (env->p) env = env->p;
    global = env;
  }

  left = 0;
  right = global->nv - 1;
  lv = global->lv;
  while (left <= right) {
    mid = (left + right) / 2;
    r = strcmp(lv[mid]->k, p);
    if (r == 0) {
      x = lv[mid]->v;
      x->r++;
      return x;
    } else if (r < 0) {
      left = mid + 1;
    } else {
      right = mid - 1;
    }
  }
  return new_errorf("unknown variable: %s", node->s);
}

static NODE*
look_func(ENV *env, const char *k) {
  NODE *x;
  static ENV *global;
  int left, right, mid, r;
  ITEM **lf;

  if (!k) return NULL;
  if (global == NULL) {
    while (env->p) env = env->p;
    global = env;
  }

  left = 0;
  right = global->nf - 1;
  lf = global->lf;
  while (left <= right) {
    mid = (left + right) / 2;
    r = strcmp(lf[mid]->k, k);
    if (r == 0) {
      x = lf[mid]->v;
      x->r++;
      return x;
    } else if (r < 0) {
      left = mid + 1;
    } else {
      right = mid - 1;
    }
  }
  return NULL;
}

static NODE*
do_call(ENV *env, NODE *node) {
  ENV *newenv;
  NODE *x = NULL, *c = NULL, *p = NULL, *nn = NULL;

  if (node->car->t == NODE_IDENT) {
    x = do_ident_global(env, node->car);
    if (x->f) {
      f_do f = (f_do) x->f;
      free_node(x);
      return f(env, node);
    }
    if (x->t != NODE_LAMBDA || x->t == NODE_ERROR) {
      free_node(x);
      x = look_func(env, node->car->s);
      if (!x) {
        return new_errorn("malformed arguments: %s", node);
      }
    }
    if (x->t == NODE_LAMBDA) {
      c = x->cdr->car;
      p = x->cdr->cdr;
      node = node->cdr;
    } else {
      c = x->cdr->cdr->car;
      p = x->cdr->cdr->cdr;
      node = node->cdr;
    }
  } else if (node->car && node->car->t == NODE_LAMBDA) {
    x = node->car;
    x->r++;
    c = x->cdr->car->car;
    p = x->cdr->cdr->car;
    node = node->cdr;
  } else {
    return new_errorn("malformed arguments: %s", node);
  }

  newenv = new_env(env);

  while (node) {
    if ((c && c->car && !strcmp("&rest", c->car->s)) || c->t == NODE_IDENT) {
      NODE *l = NULL, *rr = NULL, *nc;
      while (node) {
        nn = eval_node(env, node->car);
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
        node = node->cdr;
      }
      if (rr) add_variable(newenv, c->t == NODE_IDENT ? c->s : c->cdr->car->s, rr);
      break;
    }
    nn = eval_node(env, node->car);
    if (nn->t == NODE_ERROR) {
      free_env(newenv);
      free_node(x);
      return nn;
    }
    add_variable(newenv, c->car->s, nn);
    node = node->cdr;
    c = c->cdr;
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
do_lambda(ENV *env, NODE *node) {
  NODE *x, *xx, *c;

  if (node_narg(node) != 2) return new_errorf("malformed lambda: %s", node);

  c = x = new_node();
  x->t = NODE_LAMBDA;
  xx = node->cdr;
  xx->r++;
  while (xx) {
    x->cdr = xx;
    x = x->cdr;
    xx = xx->cdr;
  }
  return c;
}

static NODE*
do_funcall(ENV *env, NODE *node) {
  NODE *x, *xx, *c, *nn;

  if (node_narg(node) < 2) return new_errorf("malformed funcall: %s", node);

  c = x = new_node();
  x->t = NODE_CELL;
  x->car = node->cdr->car;
  x->car->r++;
  xx = node->cdr->cdr;
  xx->r++;
  while (xx) {
    x->cdr = xx;
    x = x->cdr;
    xx = xx->cdr;
  }
  nn = do_call(env, c);
  free_node(c);
  return nn;
}

static NODE*
do_defun(ENV *env, NODE *node) {
  NODE *x;

  if (node_narg(node) < 3) return new_errorn("malformed defun: %s", node);

  x = node->cdr->car;
  if (x->t != NODE_IDENT) {
    return new_errorn("invalid identifier: %s", x);
  }
  add_function(env, x->s, node);
  node->r++;
  x->r++;
  return x;
}

static NODE*
do_progn(ENV *env, NODE *node) {
  NODE *f, *c, *x, *a;

  node = node->car;
  if (!node->car) {
    return new_errorn("malformed function: %s", node);
  }
  c = NULL;
  while (node) {
    if (c) free_node(c);
    f = x = new_node();
    x->t = NODE_CELL;
    x->car = node->car;
    x->car->r++;
    a = node->car->cdr;
    if (a) a->r++;
    while (a) {
      x->cdr = a;
      x = x->cdr;
      a = a->cdr;
    }
    x = do_call(env, f);
    free_node(f);
    c = x;
    if (c->t == NODE_ERROR) break;
    node = node->cdr;
  }
  if (c) {
    return c;
  }
  return new_node();
}

static NODE*
do_dotimes(ENV *env, NODE *node) {
  ENV *newenv;
  NODE *x, *c, *nn, *err = NULL;
  int i, r;

  if (node_narg(node) != 2) return new_errorn("malformed dotimes: %s", node);

  x = node->cdr;
  c = eval_node(env, x->car->cdr->car);
  if (c->t == NODE_ERROR) return c;
  r = int_value(env, c, &err);
  free_node(c);
  if (err) return err;
  newenv = new_env(env);
  nn = new_node();
  nn->t = NODE_INT;
  add_variable(newenv, x->car->car->s, nn);
  c = NULL;
  for (i = 0; i < r; i++) {
    nn->i = i;
    if (c) free_node(c);
    c = eval_node(newenv, x->cdr->car);
    if (c->t == NODE_ERROR) break;
  }
  free_env(newenv);
  if (c) {
    return c;
  }
  return new_node();
}

static NODE*
do_type_of(ENV *env, NODE *node) {
  NODE *c;
  const char *p = "unknown";

  if (!node->cdr) return new_errorn("malformed type-of: %s", node);

  c = eval_node(env, node->cdr->car);
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
  case NODE_ERROR: p = "error"; break;
  }
  free_node(c);
  c = new_node();
  c->t = NODE_STRING;
  c->s = strdup(p);
  return c;
}

static NODE*
do_getenv(ENV *env, NODE *node) {
  NODE *c;
  const char *p;
  if (!node->cdr) return new_errorn("malformed getenv: %s", node);

  c = eval_node(env, node->cdr);
  if (c->t != NODE_STRING || !c->s) {
    free_node(c);
    return new_errorn("malformed getenv: %s", node);
  }
  p = getenv(c->s);
  if (p) {
    c = new_node();
    c->t = NODE_STRING;
    c->s = strdup(p);
    return c;
  }
  return new_node();
}

static NODE*
do_cond(ENV *env, NODE *node) {
  NODE *c, *err = NULL;
  int r;

  if (node_narg(node) < 1) return new_errorn("malformed cond: %s", node);

  c = NULL;
  node = node->cdr;
  while (node) {
    if (c) free_node(c);
    c = eval_node(env, node->car);
    if (c->t == NODE_ERROR) break;
    r = int_value(env, c, &err);
    if (err) {
      free_node(c);
      return err;
    }
    if (r != 0) {
      free_node(c);
      return eval_node(env, node->car->cdr->car);
    }
    node = node->cdr;
  }
  if (c) {
    return c;
  }
  return new_node();
}

static NODE*
do_car(ENV *env, NODE *node) {
  NODE *x, *c;

  if (node_narg(node) != 1) return new_errorn("malformed car: %s", node);

  x = eval_node(env, node->cdr->car);
  if (x->t == NODE_QUOTE) {
    free_node(x);
    c = new_node();
    c->t = NODE_IDENT;
    c->s = strdup("quote");
    return c;
  }
  if (x->t != NODE_CELL && x->t != NODE_NIL) {
    free_node(x);
    return new_errorn("argument is not a list: %s", node);
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
do_cdr(ENV *env, NODE *node) {
  NODE *x, *c;

  if (!node->cdr) return new_errorn("malformed cdr: %s", node);

  x = eval_node(env, node->cdr->car);
  if (x->t != NODE_CELL && x->t != NODE_NIL) {
    free_node(x);
    return new_errorn("argument is not a list: %s", node);
  }

  if (x->car) {
    if (x->car->cdr && !x->car->cdr->car) {
      c = new_node();
      c->t = NODE_CELL;
      c->car = x->car->cdr;
      c->car->r++;
      free_node(x);
      return c;
    }

    c = x->cdr;
    c->r++;
    free_node(x);
    return c;
  }
  free_node(x);
  return new_node();
}

static NODE*
do_rplaca(ENV *env, NODE *node) {
  NODE *lhs, *rhs;

  if (node_narg(node) != 2) return new_errorn("malformed rplaca: %s", node);

  lhs = eval_node(env, node->cdr->car);
  if (lhs->t == NODE_ERROR) return lhs;
  if (lhs->t != NODE_CELL || !lhs->cdr->cdr)
    return new_errorn("malformed rplaca: %s", node);
  rhs = eval_node(env, node->cdr->cdr->car);
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
do_rplacd(ENV *env, NODE *node) {
  NODE *lhs, *rhs;

  if (node_narg(node) != 2) return new_errorn("malformed rplacd: %s", node);

  lhs = eval_node(env, node->cdr->car);
  if (lhs->t == NODE_ERROR) return lhs;
  if (lhs->t != NODE_CELL || !lhs->cdr->cdr)
    return new_errorn("malformed rplacd: %s", node);
  rhs = eval_node(env, node->cdr->cdr->car);
  if (rhs->t == NODE_ERROR) {
    free_node(lhs);
    return rhs;
  }
  free_node(lhs->cdr);
  lhs->cdr = rhs;
  return lhs;
}

static NODE*
do_cons(ENV *env, NODE *node) {
  NODE *c, *lhs, *rhs;

  if (node_narg(node) != 2) return new_errorn("malformed cons: %s", node);

  lhs = eval_node(env, node->cdr->car);
  if (lhs->t == NODE_ERROR) return lhs;
  rhs = eval_node(env, node->cdr->cdr->car);
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
do_length(ENV *env, NODE *node) {
  NODE *x, *c;

  if (!node->cdr) return new_errorn("malformed length: %s", node);

  x = eval_node(env, node->cdr->car);
  if (x->t != NODE_CELL && x->t != NODE_NIL && x->t != NODE_STRING) {
    free_node(x);
    return new_errorn("argument is not a list: %s", node);
  }
  c = new_node();
  c->t = NODE_INT;
  c->i = x->t == NODE_NIL ? 0 : x->t == NODE_STRING ? (long)strlen(x->s) : node_length(x);
  free_node(x);
  return c;
}

static NODE*
do_concatenate(ENV *env, NODE *node) {
  NODE *x, *c, *l, *nn;

  if (node_narg(node) < 3) return new_errorn("malformed concatenate: %s", node);

  x = eval_node(env, node->cdr->car);
  if (x->t != NODE_IDENT) {
    free_node(x);
    return new_errorn("first argument is not a quote: %s", node);
  }
  l = eval_node(env, node->cdr->cdr->car);
  if (l->t != NODE_CELL && l->t != NODE_NIL && l->t != NODE_STRING) {
    free_node(x);
    return new_errorn("argument is not a list: %s", node);
  }
  c = new_node();
  c->t = !strcmp(x->s, "string") ? NODE_STRING : NODE_CELL;

  node = node->cdr->cdr;
  while (node) {
    if (c->t == NODE_STRING) {
      nn = eval_node(env, node->car);
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
    node = node->cdr;
  }
  free_node(x);
  free_node(l);
  return c;
}

static NODE*
do_make_string(ENV *env, NODE *node) {
  NODE *x, *c;

  if (node_narg(node) != 1) return new_errorn("malformed make-string: %s", node);

  x = eval_node(env, node->cdr->car);
  if (x->t == NODE_ERROR) return x;
  if (x->t != NODE_INT || x->i < 0) {
    free_node(x);
    return new_errorn("malformed make-string: %s", node);
  }
  c = new_node();
  c->t = NODE_STRING;
  c->s = (char*)malloc(x->i + 1);
  memset(c->s, ' ', x->i);
  *(c->s + x->i) = 0;
  free_node(x);
  return c;
}

static NODE*
do_load(ENV *env, NODE *node) {
  NODE *x, *ret, *top, *part;
  char *p, *t;
  long fsize;
  FILE *fp;

  if (!node->cdr) return new_errorn("malformed load: %s", node);
  x = eval_node(env, node->cdr->car);
  if (x->t == NODE_ERROR) return x;
  if (x->t != NODE_STRING) {
    free_node(x);
    return new_errorn("malformed load: %s", node);
  }

  fp = fopen(node->cdr->car->s, "rb");
  free_node(x);
  if (!fp) {
    return new_errorf("%s", strerror(errno));
  }

  fseek(fp, 0, SEEK_END);
  fsize = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  t = p = (char*)malloc(fsize + 1);
  memset(p, 0, fsize + 1);
  if (!fread(p, fsize, 1, fp)) {
    free((void*)t);
    fclose(fp);
    return new_errorf("%s", strerror(errno));
  }
  fclose(fp);

  top = new_node();
  top->t = NODE_CELL;
  p = (char*)parse_paren(top, p);
  if (!p) {
    free_node(top);
    free_node(x);
    return new_errorn("failed to load: %s", node);
  }
  p = (char*)skip_white((char*)p);
  if (*p) {
    free_node(top);
    return new_errorf("invalid token: %s", node);
  }
  free((char*)t);

  part = top;
  ret = NULL;
  while (part) {
    if (ret) free_node(ret);
    ret = eval_node(env, part);
    part = part->cdr;
  }

  free_node(top);
  if (ret->t == NODE_ERROR) {
    return ret;
  }
  free_node(ret);
  ret = new_node();
  ret->t = NODE_T;
  return ret;
}

static NODE*
do_apply(ENV *env, NODE *node) {
  NODE *f, *x, *c, *nn;

  if (node_narg(node) < 2) return new_errorn("malformed apply: %s", node);

  x = eval_node(env, node->cdr->cdr->car);
  if (x->t != NODE_CELL) {
    free_node(x);
    return new_errorn("second argument should be list: %s", node);
  }
  f = c = new_node();
  c->t = NODE_CELL;
  if (node->cdr->car->t == NODE_QUOTE)
    c->car = node->cdr->car->car;
  else
    c->car = node->cdr->car;
  c->car->r++;
  x->r++;
  while (x) {
    c->cdr = x;
    c = c->cdr;
    x = x->cdr;
  }
  nn = do_call(env, f);
  free_node(f);
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
  add_sym(env, NODE_IDENT, "dotimes", do_dotimes);
  add_sym(env, NODE_IDENT, "eq", do_eq);
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
  case NODE_QUOTE:
    c = node->car;
    c->r++;
    return c;
  case NODE_IDENT:
    return do_ident(env, node);
  case NODE_LAMBDA:
    node->r++;
    return node;
  case NODE_INT:
    node->r++;
    return node;
  case NODE_DOUBLE:
    node->r++;
    return node;
  case NODE_NIL:
    node->r++;
    return node;
  case NODE_T:
    node->r++;
    return node;
  case NODE_STRING:
    node->r++;
    return node;
  case NODE_CELL:
    c = NULL;
    if (!node->car) {
      return new_node();
    }
    if (node->car && node->car->car) {
      return eval_node(env, node->car);
    }
    if (node->car->t == NODE_IDENT || node->car->t == NODE_LAMBDA) {
      c = do_call(env, node);
    }
    if (c) return c;
    return new_errorn("illegal function call: %s", node);
  }

  return new_error("unknown node");
}

int
main(int argc, char* argv[]) {
  ENV *env;
  NODE *top, *ret, *part;
  char buf[BUFSIZ], *p;
  const char *pp;
  long fsize;

  if (argc > 1) {
    FILE *fp = fopen(argv[1], "rb");

    if (!fp) {
      fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
      exit(1);
    }
    fseek(fp, 0, SEEK_END);
    fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    p = (char*)malloc(fsize + 1);
    pp = p;
    memset(p, 0, fsize + 1);
    if (!fread(p, fsize, 1, fp)) {
      fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
      exit(1);
    }
    fclose(fp);

    env = new_env(NULL);
    add_defaults(env);
    top = new_node();
    top->t = NODE_CELL;
    p = (char*)parse_paren(top, p);
    if (!p) {
      free_node(top);
      free_env(env);
      exit(1);
    }
    free((char*)pp);
    part = top;
    ret = NULL;
    while (part) {
      ret = eval_node(env, part);
      if (ret->t == NODE_ERROR) {
        fprintf(stderr, "%s: %s\n", argv[0], ret->s);
        free_node(ret);
        break;
      }
      free_node(ret);
      part = part->cdr;
    }
    free_node(top);
    free_env(env);
    exit(0);
  }

  env = new_env(NULL);
  add_defaults(env);
  while (1) {
    if (isatty(fileno(stdin))) {
      printf("> ");
      if (!fgets(buf, sizeof(buf), stdin)) break;
      fsize = (long)strlen(buf);
      if (buf[fsize-1] == '\n') buf[fsize-1] = 0;
    } else {
      fsize = (long)fread(buf, 1, sizeof(buf), stdin);
      if (fsize <= 0) break;
      buf[fsize] = 0;
    }
    if (!strcmp(buf, "(exit)")) break;
    top = new_node();
    top->t = NODE_CELL;
    pp = parse_any(top, buf);
    if (!pp) {
      continue;
    }
    pp = skip_white(pp);
    if (*pp) {
      raise(pp);
      continue;
    }
    ret = eval_node(env, top);
    if (ret->t == NODE_ERROR) {
      fprintf(stderr, "%s: %s\n", argv[0], ret->s);
    } else if (isatty(fileno(stdin))) {
      dump_node(ret);
    }
    free_node(ret);
    free_node(top);
  }
  free_env(env);
  return 0;
}

/* vim:set et sw=2 cino=>2,\:0: */
