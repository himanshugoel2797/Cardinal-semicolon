;; Copyright (c) 2026 Himanshu Goel
;;
;; This software is released under the MIT License.
;; https://opensource.org/licenses/MIT

;; wasm-host: a WASI host runtime in Lisp (Phase 4 of notes/core/wasm-guests.md).
;; It drives a wasm32-wasi guest through the sys-wasm POLL/RESUME prims and
;; services the `wasi_snapshot_preview1` imports a clang `--target=wasm32-wasi`
;; binary needs: console I/O, and a read-only in-memory filesystem (a preopen
;; over a Lisp alist of (path . bytes), which is how Doom reads its WAD off the
;; initrd).
;;
;; THE MODEL. A guest is suspended whenever it calls a host import. The import is
;; identified by the small integer `id` assigned in `wasi-import-list`; sys-wasm
;; returns (list 'suspend id arg0 ...) and we service it, then (wasm-provide) the
;; result list and resume. Every "pointer" argument is an i32 byte offset into the
;; guest's linear memory; we reach that memory with (wasm-mem inst) -- a bytes
;; view that aliases it -- and the usual bytes-uN-ref/set! prims (little-endian,
;; the wasm convention). The view's base can move across a resume, so we re-fetch
;; (wasm-mem inst) inside every service call.
;;
;; COMPOSING. A specialised host (lisp/lib/wasm-doom.clp) needs to service its own
;; imports (display/input) alongside WASI, so the WASI side is exposed in pieces:
;; `wasi-import-list` (the import triples), `make-wasi-state` (the fd/fs/argv
;; state), `wasi-dispatch` (service one WASI id), and `run-guest` (the generic
;; poll/resume loop, parameterised by a dispatch closure).

(define-module wasm-host
  (export wasm-wasi-run
          wasi-import-list make-wasi-state wasi-dispatch run-guest
          mem-write-string)
  (import sys-wasm sys-console)

  ;; ---- WASI errno values (subset we return) ---------------------------------
  (define WASI-ESUCCESS 0)
  (define WASI-EBADF    8)
  (define WASI-EINVAL   28)
  (define WASI-ENOENT   44)
  (define WASI-ENOTSUP  58)
  (define WASI-EROFS    69)
  (define WASI-ESPIPE   70)

  ;; ---- import ids (module-name field-name id) -------------------------------
  ;; module-name is "wasi_snapshot_preview1" for every WASI preview1 import.
  ;; The ids are arbitrary but distinct; the dispatch in wasi-dispatch keys on
  ;; them. A specialised host assigns its own (cardinal) ids above 100 so they
  ;; never collide with these.
  (define WASI-FD-WRITE            1)
  (define WASI-FD-READ             2)
  (define WASI-FD-CLOSE            3)
  (define WASI-FD-SEEK             4)
  (define WASI-FD-FDSTAT-GET       5)
  (define WASI-FD-FDSTAT-SET-FLAGS 6)
  (define WASI-PROC-EXIT           7)
  (define WASI-ENVIRON-SIZES-GET   8)
  (define WASI-ENVIRON-GET         9)
  (define WASI-ARGS-SIZES-GET      10)
  (define WASI-ARGS-GET            11)
  (define WASI-CLOCK-TIME-GET      12)
  (define WASI-RANDOM-GET          13)
  (define WASI-FD-PRESTAT-GET      14)
  (define WASI-FD-PRESTAT-DIR-NAME 15)
  (define WASI-PATH-OPEN           16)
  (define WASI-PATH-CREATE-DIR     17)
  (define WASI-PATH-REMOVE-DIR     18)
  (define WASI-PATH-RENAME         19)
  (define WASI-PATH-UNLINK         20)

  (define WASI-MODULE "wasi_snapshot_preview1")

  (define (wasi-imp field id) (list WASI-MODULE field id))

  ;; The full set handed to (wasm-instantiate). A guest importing one we did NOT
  ;; list would fail to instantiate, so we list every preview1 import a normal
  ;; wasi-libc program references (a superset is fine -- unmatched ones are
  ;; ignored). The path_* mutators are stubbed (read-only FS).
  (define wasi-import-list
    (list (wasi-imp "fd_write"             WASI-FD-WRITE)
          (wasi-imp "fd_read"              WASI-FD-READ)
          (wasi-imp "fd_close"             WASI-FD-CLOSE)
          (wasi-imp "fd_seek"              WASI-FD-SEEK)
          (wasi-imp "fd_fdstat_get"        WASI-FD-FDSTAT-GET)
          (wasi-imp "fd_fdstat_set_flags"  WASI-FD-FDSTAT-SET-FLAGS)
          (wasi-imp "proc_exit"            WASI-PROC-EXIT)
          (wasi-imp "environ_sizes_get"    WASI-ENVIRON-SIZES-GET)
          (wasi-imp "environ_get"          WASI-ENVIRON-GET)
          (wasi-imp "args_sizes_get"       WASI-ARGS-SIZES-GET)
          (wasi-imp "args_get"             WASI-ARGS-GET)
          (wasi-imp "clock_time_get"       WASI-CLOCK-TIME-GET)
          (wasi-imp "random_get"           WASI-RANDOM-GET)
          (wasi-imp "fd_prestat_get"       WASI-FD-PRESTAT-GET)
          (wasi-imp "fd_prestat_dir_name"  WASI-FD-PRESTAT-DIR-NAME)
          (wasi-imp "path_open"            WASI-PATH-OPEN)
          (wasi-imp "path_create_directory" WASI-PATH-CREATE-DIR)
          (wasi-imp "path_remove_directory" WASI-PATH-REMOVE-DIR)
          (wasi-imp "path_rename"          WASI-PATH-RENAME)
          (wasi-imp "path_unlink_file"     WASI-PATH-UNLINK)))

  ;; ---- WASI state -----------------------------------------------------------
  ;; A guest's WASI world: a file-descriptor table plus the backing read-only
  ;; filesystem and the argv it sees. State is a 3-slot vector so the descriptor
  ;; positions can be mutated in place across resume slices.
  ;;   slot 0: fd table -- a 64-entry vector. #f = closed; 'con = stdio
  ;;           character device (fds 0/1/2); a file is #('file bytes pos) with the
  ;;           read cursor at index 2. fd 3 is the "/" preopen, handled by number.
  ;;   slot 1: fs -- an alist of (path-string . bytes).
  ;;   slot 2: argv -- a list of strings the guest's main() receives.
  (define FD-CAP 64)
  (define PREOPEN-FD 3)
  (define PREOPEN-NAME "/")

  (define (make-wasi-state fs argv)
    (let ((fdt (make-vector FD-CAP #f)))
      (vector-set! fdt 0 'con)
      (vector-set! fdt 1 'con)
      (vector-set! fdt 2 'con)
      (vector fdt fs argv)))

  (define (st-fdt s)  (vector-ref s 0))
  (define (st-fs s)   (vector-ref s 1))
  (define (st-argv s) (vector-ref s 2))

  (define (fd-get s fd)
    (if (and (>= fd 0) (< fd FD-CAP)) (vector-ref (st-fdt s) fd) #f))
  (define (fd-file? d) (and (vector? d) (eq? (vector-ref d 0) 'file)))

  ;; Allocate the lowest free table slot >= 4 (0..3 are stdio + the preopen).
  (define (fd-alloc s)
    (let ((fdt (st-fdt s)))
      (let loop ((i 4))
        (cond ((>= i FD-CAP) -1)
              ((vector-ref fdt i) (loop (+ i 1)))
              (else i)))))

  ;; ---- small byte/string helpers --------------------------------------------
  ;; Decode `len` bytes at `off` to a Lisp string (ASCII through, >=128 -> '?').
  (define (mem-string mem off len)
    (let loop ((i 0) (acc '()))
      (if (>= i len)
          (list->string (reverse acc))
          (let ((b (bytes-u8-ref mem (+ off i))))
            (loop (+ i 1)
                  (cons (integer->char (if (< b 128) b 63)) acc))))))

  ;; Write the bytes of string `s` at `addr`; returns the count written.
  (define (mem-write-string mem addr s)
    (let ((n (string-length s)))
      (let loop ((i 0))
        (if (< i n)
            (begin (bytes-u8-set! mem (+ addr i) (char->integer (string-ref s i)))
                   (loop (+ i 1)))
            n))))

  (define (strip-leading-slash s)
    (if (and (> (string-length s) 0)
             (= (char->integer (string-ref s 0)) 47))   ; #\/
        (substring s 1 (string-length s))
        s))

  ;; Resolve a guest path against the FS, ignoring a single leading slash on
  ;; either side (the preopen is "/", so libc hands us "doom1.wad").
  (define (fs-find fs name)
    (let ((q (strip-leading-slash name)))
      (let loop ((p fs))
        (cond ((null? p) #f)
              ((string=? (strip-leading-slash (car (car p))) q) (cdr (car p)))
              (else (loop (cdr p)))))))

  ;; ---- console output (fd 1/2) ----------------------------------------------
  (define (write-iovs mem iovs iovs-len)
    (let loop ((k 0) (total 0))
      (if (>= k iovs-len)
          total
          (let ((rec (+ iovs (* k 8))))
            (let ((buf (bytes-u32-ref mem rec))
                  (len (bytes-u32-ref mem (+ rec 4))))
              (if (> len 0)
                  (console-write (mem-string mem buf len)))
              (loop (+ k 1) (+ total len)))))))

  ;; ---- the WASI calls (each returns the result list for wasm-provide) -------

  ;; fd_write(fd, iovs, iovs_len, nwritten) -> errno
  (define (wasi-fd-write s inst args)
    (let ((fd       (car args))
          (iovs     (cadr args))
          (iovs-len (caddr args))
          (nwritten (cadddr args)))
      (if (or (= fd 1) (= fd 2))
          (let ((mem (wasm-mem inst)))
            (let ((total (write-iovs mem iovs iovs-len)))
              (console-flush)
              (bytes-u32-set! mem nwritten total)
              (list WASI-ESUCCESS)))
          (list WASI-EBADF))))

  ;; fd_read(fd, iovs, iovs_len, nread) -> errno: read from an open file's cursor
  ;; into successive iovecs; stdio/unknown fds report EOF (0 bytes).
  (define (wasi-fd-read s inst args)
    (let ((fd       (car args))
          (iovs     (cadr args))
          (iovs-len (caddr args))
          (nread    (cadddr args))
          (d        (fd-get s (car args)))
          (mem      (wasm-mem inst)))
      (if (not (fd-file? d))
          (begin (bytes-u32-set! mem nread 0) (list WASI-ESUCCESS))
          (let ((src (vector-ref d 1)))
            (let loop ((k 0) (pos (vector-ref d 2)) (total 0))
              (if (>= k iovs-len)
                  (begin (vector-set! d 2 pos)
                         (bytes-u32-set! mem nread total)
                         (list WASI-ESUCCESS))
                  (let* ((rec (+ iovs (* k 8)))
                         (buf (bytes-u32-ref mem rec))
                         (len (bytes-u32-ref mem (+ rec 4)))
                         (avail (- (bytes-length src) pos))
                         (n (if (< len avail) len (if (< avail 0) 0 avail))))
                    (if (> n 0) (bytes-copy! mem buf src pos n))
                    (loop (+ k 1) (+ pos n) (+ total n)))))))))

  ;; fd_close(fd) -> errno: free a file slot; stdio is a no-op.
  (define (wasi-fd-close s inst args)
    (let ((fd (car args)))
      (if (and (>= fd 4) (< fd FD-CAP))
          (vector-set! (st-fdt s) fd #f))
      (list WASI-ESUCCESS)))

  ;; fd_seek(fd, offset:i64, whence:i32, newoffset:i32) -> errno
  ;;   whence: 0=SET 1=CUR 2=END. Files are seekable; stdio is not (ESPIPE).
  (define (wasi-fd-seek s inst args)
    (let ((fd     (car args))
          (offset (cadr args))
          (whence (caddr args))
          (outp   (cadddr args))
          (d      (fd-get s (car args)))
          (mem    (wasm-mem inst)))
      (if (not (fd-file? d))
          (list WASI-ESPIPE)
          (let* ((sz  (bytes-length (vector-ref d 1)))
                 (cur (vector-ref d 2))
                 (base (cond ((= whence 1) cur) ((= whence 2) sz) (else 0)))
                 (np  (+ base offset)))
            (if (or (< np 0) (> np sz))
                (list WASI-EINVAL)
                (begin (vector-set! d 2 np)
                       (bytes-u64-set! mem outp np)
                       (list WASI-ESUCCESS)))))))

  ;; fd_fdstat_get(fd, retptr) -> errno: 24-byte __wasi_fdstat_t.
  ;;   u8 fs_filetype @+0 (2 CHARACTER_DEVICE for stdio, 4 REGULAR_FILE for files)
  ;;   u16 fs_flags @+2 (0); u64 fs_rights_base @+8 (all); u64 inheriting @+16 (0)
  (define (wasi-fd-fdstat-get s inst args)
    (let ((fd     (car args))
          (retptr (cadr args))
          (d      (fd-get s (car args)))
          (mem    (wasm-mem inst)))
      (if (and (not (eq? d 'con)) (not (fd-file? d)) (not (= fd PREOPEN-FD)))
          (list WASI-EBADF)
          (begin
            (let zero ((i 0))
              (if (< i 24) (begin (bytes-u8-set! mem (+ retptr i) 0) (zero (+ i 1)))))
            (bytes-u8-set! mem retptr (if (eq? d 'con) 2 (if (= fd PREOPEN-FD) 3 4)))
            (bytes-u32-set! mem (+ retptr 8)  #xFFFFFFFF)
            (bytes-u32-set! mem (+ retptr 12) #xFFFFFFFF)
            (list WASI-ESUCCESS)))))

  (define (wasi-fd-fdstat-set-flags s inst args) (list WASI-ESUCCESS))

  ;; fd_prestat_get(fd, ptr) -> errno: report fd 3 as a "/" preopen dir, EBADF for
  ;; the rest (ends libc's preopen scan). __wasi_prestat_t = u8 tag@+0 (0=dir) +
  ;; u32 pr_name_len@+4.
  (define (wasi-fd-prestat-get s inst args)
    (let ((fd  (car args))
          (ptr (cadr args))
          (mem (wasm-mem inst)))
      (if (= fd PREOPEN-FD)
          (begin (bytes-u8-set! mem ptr 0)
                 (bytes-u32-set! mem (+ ptr 4) (string-length PREOPEN-NAME))
                 (list WASI-ESUCCESS))
          (list WASI-EBADF))))

  ;; fd_prestat_dir_name(fd, ptr, len) -> errno: write the preopen's name.
  (define (wasi-fd-prestat-dir-name s inst args)
    (let ((fd  (car args))
          (ptr (cadr args))
          (mem (wasm-mem inst)))
      (if (= fd PREOPEN-FD)
          (begin (mem-write-string mem ptr PREOPEN-NAME) (list WASI-ESUCCESS))
          (list WASI-EBADF))))

  ;; path_open(dirfd, dirflags, path, path_len, oflags, rights_base:i64,
  ;;           rights_inheriting:i64, fdflags, opened_fd_ptr) -> errno
  ;; Read-only: reject create (oflags bit0); resolve the path in the FS and open a
  ;; file fd with a fresh read cursor.
  (define (wasi-path-open s inst args)
    (let ((path-ptr (caddr args))
          (path-len (cadddr args))
          (oflags   (list-ref args 4))
          (outp     (list-ref args 8))
          (mem      (wasm-mem inst)))
      (cond
        ((not (= (bitwise-and oflags 1) 0)) (list WASI-EROFS))   ; O_CREAT
        (else
         (let* ((name (mem-string mem path-ptr path-len))
                (data (fs-find (st-fs s) name)))
           (if (not data)
               (list WASI-ENOENT)
               (let ((fd (fd-alloc s)))
                 (if (< fd 0)
                     (list WASI-EBADF)
                     (begin (vector-set! (st-fdt s) fd (vector 'file data 0))
                            (bytes-u32-set! mem outp fd)
                            (list WASI-ESUCCESS))))))))))

  ;; environ/args ---------------------------------------------------------------
  (define (wasi-environ-sizes-get s inst args)
    (let ((mem (wasm-mem inst)))
      (bytes-u32-set! mem (car args) 0)
      (bytes-u32-set! mem (cadr args) 0)
      (list WASI-ESUCCESS)))
  (define (wasi-environ-get s inst args) (list WASI-ESUCCESS))

  ;; args_sizes_get(argc_ptr, bufsize_ptr): count + total bytes incl. NULs.
  (define (wasi-args-sizes-get s inst args)
    (let ((mem  (wasm-mem inst))
          (argv (st-argv s)))
      (bytes-u32-set! mem (car args) (length argv))
      (bytes-u32-set! mem (cadr args)
                      (let loop ((p argv) (n 0))
                        (if (null? p) n
                            (loop (cdr p) (+ n (string-length (car p)) 1)))))
      (list WASI-ESUCCESS)))

  ;; args_get(argv_ptr, buf_ptr): pointer array then NUL-terminated strings.
  (define (wasi-args-get s inst args)
    (let ((mem      (wasm-mem inst))
          (argv-ptr (car args))
          (buf-ptr  (cadr args))
          (argv     (st-argv s)))
      (let loop ((p argv) (i 0) (buf (cadr args)))
        (if (null? p)
            (list WASI-ESUCCESS)
            (let ((s0 (car p)))
              (bytes-u32-set! mem (+ argv-ptr (* i 4)) buf)
              (mem-write-string mem buf s0)
              (bytes-u8-set! mem (+ buf (string-length s0)) 0)
              (loop (cdr p) (+ i 1) (+ buf (string-length s0) 1)))))))

  (define (wasi-clock-time-get s inst args)
    (bytes-u64-set! (wasm-mem inst) (caddr args) 0)
    (list WASI-ESUCCESS))

  (define (wasi-random-get s inst args)
    (let ((buf (car args)) (n (cadr args)) (mem (wasm-mem inst)))
      (let loop ((i 0))
        (if (< i n) (begin (bytes-u8-set! mem (+ buf i) 0) (loop (+ i 1)))))
      (list WASI-ESUCCESS)))

  ;; ---- WASI dispatch --------------------------------------------------------
  ;; Service one WASI import. Returns the result list for (wasm-provide), or
  ;; (list 'exit code) for proc_exit. An id outside the WASI range yields
  ;; 'wasi-unknown so a composing host can fall through to its own handlers.
  (define (wasi-dispatch s inst id args)
    (cond
      ((= id WASI-FD-WRITE)            (wasi-fd-write s inst args))
      ((= id WASI-FD-READ)             (wasi-fd-read s inst args))
      ((= id WASI-FD-CLOSE)            (wasi-fd-close s inst args))
      ((= id WASI-FD-SEEK)             (wasi-fd-seek s inst args))
      ((= id WASI-FD-FDSTAT-GET)       (wasi-fd-fdstat-get s inst args))
      ((= id WASI-FD-FDSTAT-SET-FLAGS) (wasi-fd-fdstat-set-flags s inst args))
      ((= id WASI-PROC-EXIT)           (list 'exit (car args)))
      ((= id WASI-ENVIRON-SIZES-GET)   (wasi-environ-sizes-get s inst args))
      ((= id WASI-ENVIRON-GET)         (wasi-environ-get s inst args))
      ((= id WASI-ARGS-SIZES-GET)      (wasi-args-sizes-get s inst args))
      ((= id WASI-ARGS-GET)            (wasi-args-get s inst args))
      ((= id WASI-CLOCK-TIME-GET)      (wasi-clock-time-get s inst args))
      ((= id WASI-RANDOM-GET)          (wasi-random-get s inst args))
      ((= id WASI-FD-PRESTAT-GET)      (wasi-fd-prestat-get s inst args))
      ((= id WASI-FD-PRESTAT-DIR-NAME) (wasi-fd-prestat-dir-name s inst args))
      ((= id WASI-PATH-OPEN)           (wasi-path-open s inst args))
      ((= id WASI-PATH-CREATE-DIR)     (list WASI-EROFS))
      ((= id WASI-PATH-REMOVE-DIR)     (list WASI-EROFS))
      ((= id WASI-PATH-RENAME)         (list WASI-EROFS))
      ((= id WASI-PATH-UNLINK)         (list WASI-EROFS))
      ;; An id outside the WASI range: a composing host should have handled it
      ;; before falling through here, and an import not in the list can't even
      ;; instantiate -- so this is unreachable in practice. Return a valid errno
      ;; result (never a bare symbol, which run-guest would hand to wasm-provide
      ;; and kill the context).
      (else                            (list WASI-ENOTSUP))))

  ;; ---- the generic host driver loop -----------------------------------------
  ;; Poll/resume `inst` until it finishes/exits/traps, servicing each import
  ;; suspension via `dispatch` -- (lambda (id args) -> result-list | (list 'exit
  ;; code)). Returns (done code) | (exited code) | (trapped code). Does NOT tear
  ;; the instance down (the caller owns it -- a long-running guest like Doom holds
  ;; mapped surfaces past the first return).
  (define BIG-FUEL 5000000)

  (define (run-guest inst dispatch)
    (let loop ()
      (let ((r (wasm-resume inst BIG-FUEL)))
        (cond
          ((eq? r 'fuel) (loop))
          ((eq? (car r) 'done)
           (let ((results (cdr r)))
             (list 'done (if (null? results) 0 (car results)))))
          ((eq? (car r) 'trapped) (list 'trapped (cadr r)))
          ((eq? (car r) 'suspend)
           (let ((res (dispatch (cadr r) (cddr r))))
             (if (and (pair? res) (eq? (car res) 'exit))
                 (list 'exited (cadr res))
                 (begin (wasm-provide inst res) (loop)))))
          (else (list 'trapped -1))))))

  ;; ---- simple console-only entry point --------------------------------------
  ;; Run a WASI guest with no filesystem and no argv (the hello-world path).
  (define (wasm-wasi-run bytes)
    (let* ((inst  (wasm-instantiate bytes wasi-import-list))
           (state (make-wasi-state '() '("guest"))))
      (wasm-call inst "_start" (list))
      (let ((status (run-guest inst (lambda (id args)
                                      (wasi-dispatch state inst id args)))))
        (wasm-destroy inst)
        status))))
