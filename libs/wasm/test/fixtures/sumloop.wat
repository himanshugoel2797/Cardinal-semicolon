(module (func (export "sum") (param $n i32) (result i32)
  (local $i i32) (local $acc i32)
  (local.set $i (i32.const 1))
  (block $b (loop $l
    (br_if $b (i32.gt_s (local.get $i) (local.get $n)))
    (local.set $acc (i32.add (local.get $acc) (local.get $i)))
    (local.set $i (i32.add (local.get $i) (i32.const 1)))
    (br $l)))
  (local.get $acc)))
