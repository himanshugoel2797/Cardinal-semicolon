;; Exercises the 0xFC bulk-memory subops memory.copy / memory.fill and the
;; saturating float->int truncations (trunc_sat). Built with wat2wasm, which
;; emits the bulk-memory feature by default.
(module
  (memory (export "mem") 1)

  ;; memory.fill: set n bytes at dst to (val & 0xFF).
  (func (export "fill") (param $dst i32) (param $val i32) (param $n i32)
    (memory.fill (local.get $dst) (local.get $val) (local.get $n)))

  ;; memory.copy: copy n bytes src -> dst (overlap-safe).
  (func (export "copy") (param $dst i32) (param $src i32) (param $n i32)
    (memory.copy (local.get $dst) (local.get $src) (local.get $n)))

  ;; byte read/write helpers for the host to set up + read back.
  (func (export "store8") (param $a i32) (param $v i32)
    (i32.store8 (local.get $a) (local.get $v)))
  (func (export "load8") (param $a i32) (result i32)
    (i32.load8_u (local.get $a)))

  ;; saturating truncations.
  (func (export "sat_i32_f32_s") (param f32) (result i32)
    (i32.trunc_sat_f32_s (local.get 0)))
  (func (export "sat_i32_f32_u") (param f32) (result i32)
    (i32.trunc_sat_f32_u (local.get 0)))
  (func (export "sat_i32_f64_s") (param f64) (result i32)
    (i32.trunc_sat_f64_s (local.get 0)))
  (func (export "sat_i64_f64_s") (param f64) (result i64)
    (i64.trunc_sat_f64_s (local.get 0))))
