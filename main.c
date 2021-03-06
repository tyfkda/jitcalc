#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <ctype.h>

#ifdef JIT
#define eval jit_eval
#else
#define eval normal_eval
#endif

#define error(...) {fprintf(stderr, __VA_ARGS__); exit(1);}

#ifdef WIN32
  #include <windows.h>
  #define jit_memalloc(size) VirtualAlloc(0, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE)
  #define jit_memfree(p, size) VirtualFree(p, size, MEM_DECOMMIT)
#else
  #include <sys/mman.h>
  #include <dlfcn.h>
  #define jit_memalloc(size) mmap(NULL, size, PROT_EXEC|PROT_READ|PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0)
  #define jit_memfree(p, size) munmap(p, size)
#endif

#define CONCAT(a, b) a ## b
#define write_hex2(id, ...)  { uint8_t id[] = {__VA_ARGS__}; jit_write(id, sizeof(id)); }
#define write_hex1(ln, ...) write_hex2(CONCAT(_hex, ln), __VA_ARGS__)
#define write_hex(...) write_hex1(__LINE__, __VA_ARGS__)

typedef enum {
  TOKEN_PLUS,
  TOKEN_MINUS,
  TOKEN_LESSER,
  TOKEN_EQ,
  TOKEN_DOT,
  TOKEN_INTLIT,
  TOKEN_LPAREN,
  TOKEN_RPAREN,
  TOKEN_IDENT,
  TOKEN_NOTHING
} TokenKind;

typedef struct {
  TokenKind kind;
  union {
    int64_t intval;
    char* ident;
  };
} Token;

typedef enum {
  EXPR_ADD,
  EXPR_SUB,
  EXPR_LESSER,
  EXPR_INT,
  EXPR_ARG,
  EXPR_FUNCDEF,
  EXPR_FUNCCALL,
  EXPR_IF,
  EXPR_NOTHING
} ExprKind;

typedef struct _Expr {
  ExprKind kind;
  union {
    struct {
      struct _Expr* l;
      struct _Expr* r;
    } binop;
    struct {
      int64_t intval;
    } i;
    struct {
      char* fnname;
      struct _Expr* fnbody;
    } funcdef;
    struct {
      char* fnname;
      struct _Expr* callarg;
    } funcall;
    struct {
      struct _Expr* cond;
      struct _Expr* tbranch;
      struct _Expr* fbranch;
    } ifnode;
  };
} Expr;

typedef struct {
  char* name;
  Expr* fnexpr;
  size_t jitidx;
} Function;

Function funcs[1000];
int funcnum = 0;
int64_t funcarg = 0;
uint8_t* jit_mem;
size_t jit_pos = 0;

Expr* parse(void);
int64_t normal_eval(Expr* e);
int64_t jit_eval(Expr* e);

Token lex(void) {
  char c = getc(stdin);
  while (c == ' ') {
    c = getc(stdin);
  }

  if (c == ';') {
    return (Token){TOKEN_NOTHING};
  } else if (c == '\n') {
    return (Token){TOKEN_NOTHING};
  } else if (c == '+') {
    return (Token){TOKEN_PLUS};
  } else if (c == '-') {
    return (Token){TOKEN_MINUS};
  } else if (c == '<') {
    return (Token){TOKEN_LESSER};
  } else if (c == '=') {
    return (Token){TOKEN_EQ};
  } else if (c == '.') {
    return (Token){TOKEN_DOT};
  } else if (c == '(') {
    return (Token){TOKEN_LPAREN};
  } else if (c == ')') {
    return (Token){TOKEN_NOTHING};
  } else if (isdigit(c)) { // parse integer literal
    char buf[256];
    buf[0] = c;
    int i;
    for (i = 1; i < sizeof(buf) - 1; ++i) {
      char nc = getc(stdin);
      if (!isdigit(nc)) {
        ungetc(nc, stdin);
        break;
      }
      buf[i] = nc;
    }
    buf[i] = '\0';
    return (Token){TOKEN_INTLIT, {.intval = strtoll(buf, NULL, 0)}};
  } else if (isalpha(c)) { // parse identifier
    char buf[256];
    buf[0] = c;
    int i;
    for (i = 1; i < sizeof(buf) - 1; ++i) {
      char nc = getc(stdin);
      if (!isalpha(nc)) {
        ungetc(nc, stdin);
        break;
      }
      buf[i] = nc;
    }
    buf[i] = '\0';
    return (Token){TOKEN_IDENT, {.ident = strdup(buf)}};
  } else {
    return (Token){TOKEN_NOTHING};
  }
}

Expr* parse_intlit(void) {
  Token t = lex();
  switch (t.kind) {
  case TOKEN_INTLIT: {
    Expr* e = malloc(sizeof(Expr));
    e->kind = EXPR_INT;
    e->i.intval = t.intval;
    return e;
  }
  case TOKEN_LPAREN: {
    return parse();
  }
  case TOKEN_DOT: {
    Expr* e = malloc(sizeof(Expr));
    e->kind = EXPR_ARG;
    return e;
  }
  case TOKEN_IDENT: {
    if (strcmp(t.ident, "if") == 0) { // if syntax
      Expr* e = malloc(sizeof(Expr));
      e->kind = EXPR_IF;
      e->ifnode.cond = parse();
      e->ifnode.tbranch = parse();
      e->ifnode.fbranch = parse();
      return e;
    }

    Token eq = lex();
    if (eq.kind == TOKEN_EQ) { // function-def syntax
      Expr* e = malloc(sizeof(Expr));
      e->kind = EXPR_FUNCDEF;
      e->funcdef.fnname = t.ident;
      e->funcdef.fnbody = parse();
      return e;
    } else if (eq.kind == TOKEN_DOT) { // function-call syntax
      Expr* e = malloc(sizeof(Expr));
      e->kind = EXPR_FUNCCALL;
      e->funcall.fnname = t.ident;
      e->funcall.callarg = parse();
      return e;
    } else {
      assert(false);
      return NULL;
    }
  }
  default: {
    Expr* e = malloc(sizeof(Expr));
    e->kind = EXPR_NOTHING;
    return e;
  }
  }
}

Expr* parse(void) {
  Expr* l = parse_intlit();
  if (l->kind == EXPR_FUNCDEF)
    return l;
  if (l->kind == EXPR_IF)
    return l;
  for (;;) {
    Token op = lex();
    if (op.kind == TOKEN_NOTHING)
      break;
    switch (op.kind) {
    case TOKEN_PLUS: {
      Expr* r = parse_intlit();
      Expr* e = malloc(sizeof(Expr));
      e->kind = EXPR_ADD;
      e->binop.l = l;
      e->binop.r = r;
      l = e;
      break;
    }
    case TOKEN_MINUS: {
      Expr* r = parse_intlit();
      Expr* e = malloc(sizeof(Expr));
      e->kind = EXPR_SUB;
      e->binop.l = l;
      e->binop.r = r;
      l = e;
      break;
    }
    case TOKEN_LESSER: {
      Expr* r = parse_intlit();
      Expr* e = malloc(sizeof(Expr));
      e->kind = EXPR_LESSER;
      e->binop.l = l;
      e->binop.r = r;
      l = e;
      break;
    }
    default:
      assert(false);
      return NULL;
    }
  }
  return l;
}

int64_t normal_eval(Expr* e) {
  switch (e->kind) {
  case EXPR_ADD:
    return eval(e->binop.l) + eval(e->binop.r);
  case EXPR_SUB:
    return eval(e->binop.l) - eval(e->binop.r);
  case EXPR_LESSER:
    return eval(e->binop.l) < eval(e->binop.r);
  case EXPR_INT:
    return e->i.intval;
  case EXPR_ARG:
    return funcarg;
  case EXPR_FUNCDEF:
    funcs[funcnum] = (Function){e->funcdef.fnname, e->funcdef.fnbody};
    funcnum++;
    return 0;
  case EXPR_FUNCCALL:
    for (int i=0; i<funcnum; i++) {
      Function f = funcs[i];
      if (strcmp(f.name, e->funcall.fnname) == 0) {
        int64_t tmparg = funcarg;
        funcarg = eval(e->funcall.callarg);
        int64_t result = eval(f.fnexpr);
        funcarg = tmparg;
        return result;
      }
    }
    error("undeclared %s function.", e->funcall.fnname);
    break;
  case EXPR_IF: {
    int64_t c = eval(e->ifnode.cond);
    if (c) {
      return eval(e->ifnode.tbranch);
    } else {
      return eval(e->ifnode.fbranch);
    }
  }
  default:
    assert(false);
    return 0xdeadbeef;
  }
}

void jit_write(uint8_t* bytes, size_t len) {
  for (int i=0; i<len; i++) {
    jit_mem[jit_pos] = bytes[i];
    jit_pos++;
  }
}

void write_lendian(int x) {
  int b1 = x & 0xFF;
  int b2 = (x >> 8) & 0xFF;
  int b3 = (x >> 16) & 0xFF;
  int b4 = (x >> 24) & 0xFF;
  write_hex(b1, b2, b3, b4);
}

void fixup_lendian(size_t fixupidx, int x) {
  int b1 = x & 0xFF;
  int b2 = (x >> 8) & 0xFF;
  int b3 = (x >> 16) & 0xFF;
  int b4 = (x >> 24) & 0xFF;
  uint8_t* fixupaddr = jit_mem + fixupidx;
  fixupaddr[0] = b1;
  fixupaddr[1] = b2;
  fixupaddr[2] = b3;
  fixupaddr[3] = b4;
}

int64_t jit_call(void* funcp, int64_t arg) {
  int64_t result = -1;
  __asm__ volatile(
                   ".intel_syntax;"
                   "mov %%r8, %2;"
                   "call %1;"
                   "mov %0, %%rax;"
                   ".att_syntax;"
                   : "=r"(result)
                   : "r"(funcp), "r"(arg)
                   : "rax", "r8"
                   );
  return result;
}

//
// stack machine jit codegen, r8 is special register for dot(.) argument.
//
void jit_codegen(Expr* e) {
  switch (e->kind) {
  case EXPR_ADD:
    jit_codegen(e->binop.l);
    jit_codegen(e->binop.r);
    write_hex(
              0x59, // pop rcx
              0x58, // pop rax
              0x48, 0x01, 0xc8, // add rax, rcx
              0x50 // push rax
              );
    break;
  case EXPR_SUB:
    jit_codegen(e->binop.l);
    jit_codegen(e->binop.r);
    write_hex(
              0x59, // pop rcx
              0x58, // pop rax
              0x48, 0x29, 0xc8, // sub rax, rcx
              0x50 // push rax
              );
    break;
  case EXPR_LESSER:
    jit_codegen(e->binop.l);
    jit_codegen(e->binop.r);
    write_hex(
              0x59, // pop rcx
              0x58, // pop rax
              0x48, 0x39, 0xc8, // cmp rax, rcx
              0x0f, 0x9c, 0xc0, // setl al
              0x48, 0x0f, 0xb6, 0xc0, // movzx rax, al
              0x50 // push rax
              );
    break;
  case EXPR_INT:
    write_hex(0x68); // push $intlit
    write_lendian(e->i.intval);
    break;
  case EXPR_ARG:
    write_hex(0x41, 0x50); // push r8
    break;
  case EXPR_FUNCDEF:
    funcs[funcnum] = (Function){e->funcdef.fnname, e->funcdef.fnbody, jit_pos};
    funcnum++;
    jit_codegen(e->funcdef.fnbody);
    write_hex(0x58); // pop rax # for return value.
    write_hex(0xc3); // ret
    break;
  case EXPR_FUNCCALL:
    for (int i=0; i<funcnum; i++) {
      Function f = funcs[i];
      if (strcmp(f.name, e->funcall.fnname) == 0) {
        write_hex(0x41, 0x50); // push r8 # for register escape
        jit_codegen(e->funcall.callarg);
        write_hex(0x41, 0x58); // pop r8 # for dot(.) argument.
        write_hex(0xe8); // call $rel
        write_lendian(f.jitidx - jit_pos - 4);
        write_hex(
                  0x41, 0x58, // pop r8 # for restore register.
                  0x50 // push rax
                  );
        return;
      }
    }
    error("undeclared %s function.", e->funcall.fnname);
    break;
  case EXPR_IF:
    jit_codegen(e->ifnode.cond);
    write_hex(0x58); // pop rax
    write_hex(0x48, 0x83, 0xf8, 0x00); // cmp rax, 0
    write_hex(0x0f, 0x84); write_lendian(0); size_t fixupF = jit_pos; // je F (fixup)
    jit_codegen(e->ifnode.tbranch); // - true branch
    write_hex(0xe9); write_lendian(0); size_t fixupE = jit_pos; // jmp E (fixup)
    size_t faddr = jit_pos; // F:
    jit_codegen(e->ifnode.fbranch); // - false branch
    size_t eaddr = jit_pos; // E:
    fixup_lendian(fixupF-4, faddr - fixupF);
    fixup_lendian(fixupE-4, eaddr - fixupE);
    break;
  default:
    assert(false);
    break;
  }
}

int64_t jit_eval(Expr* e) {
  if (e->kind == EXPR_FUNCDEF) {
    jit_codegen(e);
    return 0;
  } else if (e->kind == EXPR_FUNCCALL) {
    for (int i=0; i<funcnum; i++) {
      Function f = funcs[i];
      if (strcmp(f.name, e->funcall.fnname) == 0) {
        return jit_call(jit_mem + f.jitidx, jit_eval(e->funcall.callarg));
      }
    }
    error("undeclared %s function.", e->funcall.fnname);
  } else {
    return normal_eval(e);
  }
}

int main() {
  jit_mem = jit_memalloc(1024*1024);
  for (;;) {
    Expr* e = parse();
    if (e->kind == EXPR_NOTHING)
      break;
    printf("%"PRId64" ", eval(e));
  }
  printf("\n");
  return 0;
}
