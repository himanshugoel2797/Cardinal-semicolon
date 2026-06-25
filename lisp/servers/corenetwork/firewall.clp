;; corenetwork/firewall: a small inbound packet filter. An ordered list of
;; allow/deny rules matched on (protocol, source IP/prefix, destination port) plus
;; a default policy; evaluated on every received IP packet BEFORE it is dispatched
;; to ICMP/UDP/TCP. First matching rule wins; if none matches, the default policy
;; decides. This is a HOST inbound filter (we do not forward), so a rule matches on
;; the SOURCE address -- which, since interfaces sit on distinct subnets, also
;; stands in for "which interface it arrived on" without ingress tagging.
;;
;; Default policy is ALLOW with no rules, so the firewall is inert until
;; configured -- existing behaviour is unchanged unless rules are installed.

;; A firewall is a mutable vector #(default-allow? rules); it lives in the service
;; closure (not the threaded state) and is mutated in place by config messages.
(define FW-DEFAULT 0) (define FW-RULES 1)
(define (mk-fw) (vector #t '()))

;; A rule is (action proto src-net src-len dport). `action` is 'allow or 'deny;
;; `proto` is an IP protocol number or 'any; `src-net`/`src-len` are an IP prefix
;; (or 'any matches every source); `dport` is a destination port or 'any. ICMP has
;; no port, so an ICMP rule uses dport 'any.
(define (fw-rule action proto src-net src-len dport)
  (list action proto src-net src-len dport))
(define (fw-action r) (car r))

(define (fw-rule-match? r proto src-ip dport)
  (let ((rp (cadr r)) (rsrc (caddr r)) (rlen (cadddr r)) (rdp (nth r 4)))
    (and (or (eq? rp 'any) (= rp proto))
         (or (eq? rsrc 'any) (ip-prefix-eq? rsrc src-ip rlen))
         (or (eq? rdp 'any) (= rdp dport)))))

;; The verdict for a received packet: #t = allow (deliver), #f = deny (drop).
(define (fw-allow? fw proto src-ip dport)
  (let loop ((rs (vector-ref fw FW-RULES)))
    (cond ((null? rs) (vector-ref fw FW-DEFAULT))           ; no rule matched: default
          ((fw-rule-match? (car rs) proto src-ip dport)
           (eq? (fw-action (car rs)) 'allow))
          (else (loop (cdr rs))))))

;; Config (mutate in place). Rules are appended, so they are evaluated in the
;; order added; clear resets to the empty list (default policy then governs).
(define (fw-add! fw rule)
  (vector-set! fw FW-RULES (append (vector-ref fw FW-RULES) (list rule))))
(define (fw-set-policy! fw allow?) (vector-set! fw FW-DEFAULT allow?))
(define (fw-clear! fw) (vector-set! fw FW-RULES '()))
