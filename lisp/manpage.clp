;; manpage: the in-OS documentation browser -- `man` and `apropos` over the REPL.
;;
;; The hand-written half of the Cardinal; doc system (the generated half is
;; docs-db, written by scripts/docs/extract.py). It (import docs-db)s the one
;; quoted list of documented symbols and serves two readers over the serial REPL:
;; `(man 'sym)` prints a formatted page for a single symbol, `(apropos "text")`
;; lists every symbol whose name or brief contains a substring. The freshness
;; status the extractor stamped (current / stale / missing-source / n/a) rides in
;; each entry, so `man` can flag a page whose code has drifted out from under it.
;;
;; Each doc-entry is the 7-element list extract.py emits:
;;   (name-string kind-symbol lang-symbol source-string status-symbol
;;    brief-string body-string)

(define-module manpage
  (export man apropos)
  (import docs-db)

  ;; --- field accessors over a doc-entry list --------------------------------
  ;; (the language has c[ad]+r up to four deep but no cddddr, so the index-4..6
  ;;  fields are reached by composing the available accessors.)
  (define (entry-name e)   (car e))               ; 0
  (define (entry-kind e)   (cadr e))              ; 1
  (define (entry-lang e)   (caddr e))             ; 2
  (define (entry-source e) (cadddr e))            ; 3
  (define (entry-status e) (cadddr (cdr e)))      ; 4
  (define (entry-brief e)  (cadddr (cddr e)))     ; 5
  (define (entry-body e)   (car (cdddr (cdddr e)))) ; 6

  ;; Coerce a symbol-or-string query to a string (man accepts either form, so
  ;; both `(man 'physmem_alloc)` and `(man "physmem_alloc")` work).
  (define (as-string x)
    (if (string? x) x (symbol->string x)))

  ;; --- substring? : is `needle` a substring of `hay`? -----------------------
  ;; The substring primitive + string=? give us a sliding-window match; there is
  ;; no built-in string-search, so apropos and the name lookup share this.
  (define (substring-at? hay needle start nlen)
    (string=? (substring hay start (+ start nlen)) needle))

  (define (substring? hay needle)
    (let ((hl (string-length hay))
          (nl (string-length needle)))
      (if (= nl 0)
          #t
          (let loop ((i 0))
            (cond ((> (+ i nl) hl) #f)
                  ((substring-at? hay needle i nl) #t)
                  (else (loop (+ i 1))))))))

  ;; --- entry lookup by exact name -------------------------------------------
  (define (find-entry name entries)
    (cond ((null? entries) #f)
          ((string=? (entry-name (car entries)) name) (car entries))
          (else (find-entry name (cdr entries)))))

  ;; --- formatted output helpers ---------------------------------------------
  (define (line label val)
    (display label) (display val) (newline))

  ;; The [doc current] / [doc STALE] freshness line. current/n-a read as healthy;
  ;; anything else (stale, missing-source) is called out so the reader distrusts
  ;; the page until a human re-verifies and re-stamps it.
  (define (status-line status)
    (cond ((eq? status 'current)        (line "  " "[doc current]"))
          ((eq? status 'n/a)            (line "  " "[doc overview]"))
          ((eq? status 'missing-source) (line "  " "[doc MISSING-SOURCE]"))
          (else                         (line "  " "[doc STALE]"))))

  ;; --- man : print the page for one symbol ----------------------------------
  (define (man sym)
    (let* ((name (as-string sym))
           (e    (find-entry name doc-entries)))
      (if (not e)
          (begin (display "no manual entry for ") (display name) (newline))
          (begin
            (line "NAME    " (entry-name e))
            (line "KIND    " (symbol->string (entry-kind e)))
            (line "SOURCE  " (entry-source e))
            (status-line (entry-status e))
            (newline)
            (display (entry-brief e)) (newline)
            (newline)
            (display (entry-body e)) (newline)))))

  ;; --- apropos : list every entry whose name or brief contains `str` --------
  (define (apropos str)
    (let loop ((es doc-entries) (hits 0))
      (cond
        ((null? es)
         (if (= hits 0)
             (begin (display "apropos: nothing matches ") (display str) (newline))))
        ((or (substring? (entry-name (car es)) str)
             (substring? (entry-brief (car es)) str))
         (display (entry-name (car es)))
         (display " - ")
         (display (entry-brief (car es)))
         (newline)
         (loop (cdr es) (+ hits 1)))
        (else (loop (cdr es) hits))))))
