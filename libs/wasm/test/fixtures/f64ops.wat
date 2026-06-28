(module (func (export "hyp") (param f64 f64) (result f64)
  (f64.sqrt (f64.add (f64.mul (local.get 0)(local.get 0))
                     (f64.mul (local.get 1)(local.get 1))))))
