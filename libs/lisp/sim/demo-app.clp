;; demo-app: a tiny user application for the simulator -- proof that an app can
;; run on the host against the real coreinput service and a fake display driver,
;; reacting to host keyboard/mouse with no VM.
;;
;; It draws a window-ish scene (background + title bar + a movable box) into its
;; OWN surface and sends the finished frame to the display driver to present.
;; Input arrives as the coreinput envelope (input <ev>):
;;   arrow keys (PS/2 set-1 0x48/0x50/0x4B/0x4D) move the box;
;;   space (0x39) cycles its colour;
;;   a pointer press recentres the box on the cursor.
;; Every change repaints and re-presents, so the host window tracks the input
;; live (and the offscreen backend's final PPM encodes the last state, which the
;; harness test asserts on).

(define-module demo-app
  (export run-demo-app)
  (import driver-util graphics)

  (define BOX 48)
  (define STEP 24)
  ;; PS/2 set-1 scancodes (see sim/keymap.c).
  (define K-UP #x48) (define K-DOWN #x50) (define K-LEFT #x4B) (define K-RIGHT #x4D)
  (define K-SPACE #x39)
  (define PALETTE (list (list 235 80 80) (list 90 220 110)
                        (list 90 160 245) (list 240 215 90)))

  (define (clamp v lo hi) (cond ((< v lo) lo) ((> v hi) hi) (else v)))

  ;; Paint the whole scene into `surf` (over `fb`) and present it.
  (define (repaint disp fb surf w h bx by ci)
    (clear surf (rgb surf 20 24 40))                    ; desktop background
    (fill-rect surf 0 0 w 28 (rgb surf 40 60 110))      ; title bar
    (fill-rect surf 6 8 12 12 (rgb surf 235 235 245))   ; a little title-bar glyph
    (let ((c (nth PALETTE ci)))
      (fill-rect surf bx by BOX BOX (rgb surf (car c) (cadr c) (caddr c))))
    (draw-rect surf bx by BOX BOX 2 (rgb surf 255 255 255))  ; box outline
    (send disp (list 'present fb)))

  ;; Spawn the app. `coreinput` is the input service; `disp` the display driver.
  (define (run-demo-app coreinput disp)
    (spawn
      (lambda ()
        (send coreinput (list 'subscribe (self)))
        (send disp (list 'get-info (self)))
        (let ((info (recv)))                            ; (w h stride)
          (let* ((w (nth info 0)) (h (nth info 1)) (stride (nth info 2))
                 (fb (make-bytes (* stride h)))
                 (surf (make-surface fb w h stride))
                 (cx (clamp (quotient (- w BOX) 2) 0 (- w BOX)))
                 (cy (clamp (quotient (- h BOX) 2) 28 (- h BOX))))
            (let loop ((bx cx) (by cy) (ci 0))
              (repaint disp fb surf w h bx by ci)
              (let ((m (recv)))
                (if (and (pair? m) (eq? (car m) 'input) (pair? (cdr m)))
                    (let ((ev (cadr m)))
                      (cond
                        ;; key press
                        ((and (pair? ev) (eq? (car ev) 'key)
                              (>= (length ev) 3) (= (nth ev 2) 1))
                         (let ((sc (nth ev 1)))
                           (cond ((= sc K-RIGHT) (loop (clamp (+ bx STEP) 0 (- w BOX)) by ci))
                                 ((= sc K-LEFT)  (loop (clamp (- bx STEP) 0 (- w BOX)) by ci))
                                 ((= sc K-DOWN)  (loop bx (clamp (+ by STEP) 28 (- h BOX)) ci))
                                 ((= sc K-UP)    (loop bx (clamp (- by STEP) 28 (- h BOX)) ci))
                                 ((= sc K-SPACE) (loop bx by (if (>= (+ ci 1) (length PALETTE)) 0 (+ ci 1))))
                                 (else (loop bx by ci)))))
                        ;; pointer press -> recentre the box on the cursor
                        ((and (pair? ev) (eq? (car ev) 'pointer)
                              (>= (length ev) 4) (nth ev 3))
                         (loop (clamp (- (nth ev 1) (quotient BOX 2)) 0 (- w BOX))
                               (clamp (- (nth ev 2) (quotient BOX 2)) 28 (- h BOX))
                               ci))
                        (else (loop bx by ci))))
                    (loop bx by ci))))))))))
