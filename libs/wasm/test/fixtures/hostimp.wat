(module (import "env" "dbl" (func $dbl (param i32) (result i32)))
  (func (export "calldbl") (param i32) (result i32)
    (call $dbl (local.get 0))))
