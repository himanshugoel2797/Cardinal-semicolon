(module
  (memory (export "mem") 1 1)
  (func (export "add") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.add)
  (func (export "store") (param i32 i32)
    local.get 0 local.get 1 i32.store))
