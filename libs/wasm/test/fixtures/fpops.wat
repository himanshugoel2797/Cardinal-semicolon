;; Exercises the freestanding IEEE-754 math helpers (floor/ceil/trunc/nearest/
;; sqrt + copysign) so the committed host suite covers wm_* in wasm_exec.c.
(module
  (func (export "floor") (param f64) (result f64) (f64.floor (local.get 0)))
  (func (export "ceil")  (param f64) (result f64) (f64.ceil  (local.get 0)))
  (func (export "trunc") (param f64) (result f64) (f64.trunc (local.get 0)))
  (func (export "nearest") (param f64) (result f64) (f64.nearest (local.get 0)))
  (func (export "sqrt")  (param f64) (result f64) (f64.sqrt  (local.get 0)))
  (func (export "copysign") (param f64 f64) (result f64)
        (f64.copysign (local.get 0) (local.get 1)))
  (func (export "ffloor") (param f32) (result f32) (f32.floor (local.get 0)))
  (func (export "fnearest") (param f32) (result f32) (f32.nearest (local.get 0))))
