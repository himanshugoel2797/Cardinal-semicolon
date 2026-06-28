;; Copyright (c) 2026 Himanshu Goel
;;
;; This software is released under the MIT License.
;; https://opensource.org/licenses/MIT

;; wasm-host: a minimal WASI host runtime in Lisp (Phase 4a of
;; notes/core/wasm-guests.md). It drives a wasm32-wasi guest through the sys-wasm
;; POLL/RESUME prims and services the `wasi_snapshot_preview1` imports a small C
;; program (hello-world) needs, so a clang `--target=wasm32-wasi` binary can run
;; on the in-OS interpreter and do console I/O.
;;
;; THE MODEL. A guest is suspended whenever it calls a host import. The import is
;; identified by the small integer `id` assigned in `wasi-import-list`; sys-wasm
;; returns (list 'suspend id arg0 ...) and we service it, then (wasm-provide) the
;; result list and resume. Every "pointer" argument is an i32 byte offset into the
;; guest's linear memory; we reach that memory with (wasm-mem inst) -- a bytes
;; view that aliases it -- and the usual bytes-uN-ref/set! prims (little-endian,
;; the wasm convention). The view's base can move across a resume, so we re-fetch
;; (wasm-mem inst) inside every service call.

(define-module wasm-host
  (export wasm-wasi-run wasi-import-list)
  (import sys-wasm sys-console)

  ;; ---- WASI errno values (subset we return) ---------------------------------
  (define WASI-ESUCCESS 0)
  (define WASI-EBADF    8)
  (define WASI-EINVAL   28)
  (define WASI-ENOTSUP  58)
  (define WASI-ESPIPE   70)

  ;; ---- import ids (module-name field-name id) -------------------------------
  ;; module-name is "wasi_snapshot_preview1" for every WASI preview1 import.
  ;; The ids are arbitrary but distinct; the dispatch in wasi-service keys on
  ;; them. Passing a superset of what a module declares is fine -- wasm-instantiate
  ;; only matches the imports the module actually imports, and ignores the rest;
  ;; a guest importing one we did NOT list would fail to instantiate, so we err on
  ;; the side of listing the common stubs too.
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

  (define WASI-MODULE "wasi_snapshot_preview1")

  (define (wasi-imp field id) (list WASI-MODULE field id))

  ;; The full set handed to (wasm-instantiate). hello-world needs fd_write,
  ;; fd_close, fd_fdstat_get, fd_seek, proc_exit; the rest are common stubs so
  ;; more guests instantiate.
  (define wasi-import-list
    (list (wasi-imp "fd_write"            WASI-FD-WRITE)
          (wasi-imp "fd_read"             WASI-FD-READ)
          (wasi-imp "fd_close"            WASI-FD-CLOSE)
          (wasi-imp "fd_seek"             WASI-FD-SEEK)
          (wasi-imp "fd_fdstat_get"       WASI-FD-FDSTAT-GET)
          (wasi-imp "fd_fdstat_set_flags" WASI-FD-FDSTAT-SET-FLAGS)
          (wasi-imp "proc_exit"           WASI-PROC-EXIT)
          (wasi-imp "environ_sizes_get"   WASI-ENVIRON-SIZES-GET)
          (wasi-imp "environ_get"         WASI-ENVIRON-GET)
          (wasi-imp "args_sizes_get"      WASI-ARGS-SIZES-GET)
          (wasi-imp "args_get"            WASI-ARGS-GET)
          (wasi-imp "clock_time_get"      WASI-CLOCK-TIME-GET)
          (wasi-imp "random_get"          WASI-RANDOM-GET)))

  ;; ---- iovec / output marshalling -------------------------------------------
  ;; A ciovec record is 8 bytes: buf=u32@+0, len=u32@+4. Decode `len` bytes at
  ;; `buf` to a Lisp string -- ASCII bytes (0..127) map straight through with
  ;; integer->char, anything >=128 folds to '?', the same trick coreusb uses for
  ;; descriptor strings. We never blit raw memory through the ABI; the bytes are
  ;; copied into a string and console-written.
  (define (mem-string mem buf len)
    (let loop ((i 0) (acc '()))
      (if (>= i len)
          (list->string (reverse acc))
          (let ((b (bytes-u8-ref mem (+ buf i))))
            (loop (+ i 1)
                  (cons (integer->char (if (< b 128) b 63)) acc))))))

  ;; Write each of `iovs-len` iovecs (8 bytes apart) starting at `iovs` to the
  ;; console; return the total byte count written.
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
  ;; All take the live arg list already stripped of `id`; pointers are i32
  ;; offsets into (wasm-mem inst), re-fetched here since the base can move.

  ;; fd_write(fd, iovs, iovs_len, nwritten) -> errno
  (define (wasi-fd-write inst args)
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

  ;; fd_read(fd, iovs, iovs_len, nread) -> errno: no stdin, report EOF (0 bytes).
  (define (wasi-fd-read inst args)
    (let ((nread (cadddr args))
          (mem   (wasm-mem inst)))
      (bytes-u32-set! mem nread 0)
      (list WASI-ESUCCESS)))

  ;; fd_close(fd) -> errno
  (define (wasi-fd-close inst args)
    (list WASI-ESUCCESS))

  ;; fd_seek(fd, offset:i64, whence:i32, newoffset:i32) -> errno
  ;; stdout/stderr are not seekable -> ESPIPE.
  (define (wasi-fd-seek inst args)
    (list WASI-ESPIPE))

  ;; fd_fdstat_get(fd, retptr) -> errno: write a 24-byte __wasi_fdstat_t.
  ;;   u8  fs_filetype        @+0  (2 CHARACTER_DEVICE for 0/1/2, else 4 REGULAR_FILE)
  ;;   u16 fs_flags           @+2  (0)
  ;;   u64 fs_rights_base     @+8  (all rights)
  ;;   u64 fs_rights_inheriting @+16 (0)
  ;; The struct is zeroed first, then the live fields are set.
  (define (wasi-fd-fdstat-get inst args)
    (let ((fd     (car args))
          (retptr (cadr args))
          (mem    (wasm-mem inst)))
      (let zero ((i 0))
        (if (< i 24)
            (begin (bytes-u8-set! mem (+ retptr i) 0) (zero (+ i 1)))))
      (bytes-u8-set! mem retptr
                     (if (or (= fd 0) (= fd 1) (= fd 2)) 2 4))
      ;; fs_rights_base = all-ones (every right); split across two u32 halves
      ;; because the value exceeds the Lisp fixnum range.
      (bytes-u32-set! mem (+ retptr 8)  #xFFFFFFFF)
      (bytes-u32-set! mem (+ retptr 12) #xFFFFFFFF)
      (list WASI-ESUCCESS)))

  ;; fd_fdstat_set_flags(fd, flags) -> errno
  (define (wasi-fd-fdstat-set-flags inst args)
    (list WASI-ESUCCESS))

  ;; environ_sizes_get(count_ptr, bufsize_ptr) -> errno: no environment.
  (define (wasi-environ-sizes-get inst args)
    (let ((count-ptr   (car args))
          (bufsize-ptr (cadr args))
          (mem (wasm-mem inst)))
      (bytes-u32-set! mem count-ptr 0)
      (bytes-u32-set! mem bufsize-ptr 0)
      (list WASI-ESUCCESS)))

  ;; environ_get(environ_ptr, buf_ptr) -> errno: nothing to write.
  (define (wasi-environ-get inst args)
    (list WASI-ESUCCESS))

  ;; args_sizes_get(argc_ptr, bufsize_ptr) -> errno: no args.
  (define (wasi-args-sizes-get inst args)
    (let ((argc-ptr    (car args))
          (bufsize-ptr (cadr args))
          (mem (wasm-mem inst)))
      (bytes-u32-set! mem argc-ptr 0)
      (bytes-u32-set! mem bufsize-ptr 0)
      (list WASI-ESUCCESS)))

  ;; args_get(argv_ptr, buf_ptr) -> errno: nothing to write.
  (define (wasi-args-get inst args)
    (list WASI-ESUCCESS))

  ;; clock_time_get(clock_id, precision:i64, time_ptr) -> errno: write a u64
  ;; nanosecond value. We have no monotonic source importable under this module's
  ;; capability set, so write 0 (a real clock is a later concern).
  (define (wasi-clock-time-get inst args)
    (let ((time-ptr (caddr args))
          (mem (wasm-mem inst)))
      (bytes-u64-set! mem time-ptr 0)
      (list WASI-ESUCCESS)))

  ;; random_get(buf, buf_len) -> errno: fill buf with bytes. Zeros are acceptable
  ;; for v1; a real entropy source is a later concern.
  (define (wasi-random-get inst args)
    (let ((buf     (car args))
          (buf-len (cadr args))
          (mem (wasm-mem inst)))
      (let loop ((i 0))
        (if (< i buf-len)
            (begin (bytes-u8-set! mem (+ buf i) 0) (loop (+ i 1)))))
      (list WASI-ESUCCESS)))

  ;; ---- dispatch -------------------------------------------------------------
  ;; Return the result list to (wasm-provide), or (list 'exit code) for proc_exit
  ;; (the loop surfaces the code and tears the instance down). Any id we did not
  ;; implement returns ENOTSUP so the guest fails gracefully rather than wedging.
  (define (wasi-service inst id args)
    (cond
      ((= id WASI-FD-WRITE)            (wasi-fd-write inst args))
      ((= id WASI-FD-READ)             (wasi-fd-read inst args))
      ((= id WASI-FD-CLOSE)            (wasi-fd-close inst args))
      ((= id WASI-FD-SEEK)             (wasi-fd-seek inst args))
      ((= id WASI-FD-FDSTAT-GET)       (wasi-fd-fdstat-get inst args))
      ((= id WASI-FD-FDSTAT-SET-FLAGS) (wasi-fd-fdstat-set-flags inst args))
      ((= id WASI-PROC-EXIT)           (list 'exit (car args)))
      ((= id WASI-ENVIRON-SIZES-GET)   (wasi-environ-sizes-get inst args))
      ((= id WASI-ENVIRON-GET)         (wasi-environ-get inst args))
      ((= id WASI-ARGS-SIZES-GET)      (wasi-args-sizes-get inst args))
      ((= id WASI-ARGS-GET)            (wasi-args-get inst args))
      ((= id WASI-CLOCK-TIME-GET)      (wasi-clock-time-get inst args))
      ((= id WASI-RANDOM-GET)          (wasi-random-get inst args))
      (else                            (list WASI-ENOTSUP))))

  ;; ---- the host driver loop -------------------------------------------------
  ;; Instantiate `bytes`, run "_start", then poll/resume until the guest is done,
  ;; exits, or traps; service each import suspension in between. Returns a small
  ;; status value and always tears the instance down before returning:
  ;;   (list 'done  exit-code)   -- _start returned normally (exit-code is the
  ;;                                first i32 result, or 0 if none)
  ;;   (list 'exited code)       -- the guest called proc_exit(code)
  ;;   (list 'trapped code)      -- the guest trapped (wasm_trap_t code)

  (define BIG-FUEL 5000000)

  (define (run-loop inst)
    (let loop ()
      (let ((r (wasm-resume inst BIG-FUEL)))
        (cond
          ;; fuel exhausted: keep going.
          ((eq? r 'fuel) (loop))
          ;; (cons 'done results): _start returned. Surface the first i32 result
          ;; as the exit code if the guest produced one, else 0.
          ((eq? (car r) 'done)
           (let ((results (cdr r)))
             (list 'done (if (null? results) 0 (car results)))))
          ;; (list 'trapped code)
          ((eq? (car r) 'trapped)
           (list 'trapped (cadr r)))
          ;; (list 'suspend id arg0 ...): service the import.
          ((eq? (car r) 'suspend)
           (let ((id   (cadr r))
                 (args (cddr r)))
             (let ((res (wasi-service inst id args)))
               (if (and (pair? res) (eq? (car res) 'exit))
                   (list 'exited (cadr res))
                   (begin (wasm-provide inst res) (loop))))))
          ;; an unexpected status shape: treat as a trap with no code.
          (else (list 'trapped -1))))))

  (define (wasm-wasi-run bytes)
    (let ((inst (wasm-instantiate bytes wasi-import-list)))
      (wasm-call inst "_start" (list))
      (let ((status (run-loop inst)))
        (wasm-destroy inst)
        status))))
