# Special forms

*Part of the [Lisp VM Reference](index.md).*

The bytecode compiler (`lbc.c`) recognizes the following special forms.
Everything else is a procedure call.

## `(quote datum)` / `'datum`

Returns `datum` unevaluated.

```scheme
'(1 2 3)     ; => (1 2 3)
'foo         ; => foo (the symbol)
```

## `(quasiquote template)` / `` `template ``

Backquote template: nested `unquote` and `unquote-splicing` are evaluated;
everything else is quoted.

```scheme
(let ((x 5)) `(the value is ,x))    ; => (the value is 5)
(let ((xs '(2 3))) `(1 ,@xs 4))     ; => (1 2 3 4)
```

## `(if test consequent [alternative])`

Evaluates `test`; if truthy, evaluates and returns `consequent`, else
`alternative` (or `#<undef>` if omitted).  Only `#f` is falsy.

## `(cond (test expr ...) ... [(else expr ...)])`

Evaluates each `test` in order; the first truthy clause's body is evaluated.
`(test => proc)` applies `proc` to the test value.  The `else` clause is a
catch-all.

## `(when test body ...)`

Evaluates `body ...` as a `begin` when `test` is truthy; returns `#<undef>`
when false.

## `(unless test body ...)`

Like `when` but evaluates body when `test` is falsy.

## `(and expr ...)`

Evaluates left-to-right; returns the first falsy value or the last value.
`(and)` returns `#t`.

## `(or expr ...)`

Returns the first truthy value or the last value.  `(or)` returns `#f`.

## `(begin expr ...)`

Evaluates expressions left-to-right; returns the value of the last one.

## `(define name expr)` / `(define (name params ...) body ...)`

Binds `name` in the current environment.  The procedure shorthand desugars to
`(define name (lambda (params ...) body ...))`.  Inside a `lambda` or `let`
body, `define` forms create local bindings visible throughout the body (internal
defines).

## `(set! name expr)`

Mutates an existing binding (must already be defined); errors if `name` is
unbound.

## `(lambda (params ...) body ...)` / `(lambda (params ... . rest) body ...)`

Creates a closure.  A trailing `. rest` parameter collects excess arguments
into a list.  `(lambda args body ...)` collects all arguments.

## `(let ((var init) ...) body ...)`

Bind variables (all `init`s evaluated before any binding is visible), then
evaluate `body ...` in the extended environment.

## `(let* ((var init) ...) body ...)`

Like `let` but each `init` can refer to the preceding bindings.

## `(letrec ((var init) ...) body ...)`

Mutual-recursion binding: all `var`s are bound before any `init` is evaluated
(init expressions may reference each other).

## `(let loop-name ((var init) ...) body ...)` — named let

Named-let / looping construct.  `loop-name` is bound to a procedure that
re-enters the loop with new values.

```scheme
(let loop ((i 0) (acc 0))
  (if (> i 5) acc (loop (+ i 1) (+ acc i))))   ; => 15
```

## `(while test body ...)`

Repeatedly evaluates `body ...` as long as `test` is truthy; returns
`#<undef>`.

```scheme
(let ((i (make-vector 1 0)))
  (while (< (vector-ref i 0) 5)
    (vector-set! i 0 (+ (vector-ref i 0) 1))))
```

## `(case key (datums expr ...) ... [(else expr ...)])`

Evaluates `key`; compares with each clause's datum list using `eqv?`; evaluates
the first matching clause's body.  `else` is a catch-all.

```scheme
(case x
  ((1 2) "small")
  ((3 4) "medium")
  (else  "large"))
```

## `(define-module name (export sym ...) body ...)`

Defines a module (see [Modules & capabilities](capabilities.md)).  Root-context only.

## `(import spec ...)` / `(include part ...)`

Loads and binds a module's exports (see [Modules & capabilities](capabilities.md)).
