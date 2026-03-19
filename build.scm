#! /usr/local/bin/guile -s
!#

(add-to-load-path ".")
(use-modules (buildlib))

(define install? (memq #t (map (lambda (x) (equal? "install" x)) (command-line))))
(define clean? (memq #t (map (lambda (x) (equal? "clean" x)) (command-line))))
(define compile? (not clean?))

;; Assumes that you already have glfw installed
(let ((config (configure #:exe-name "deity"
                         #:link '("glfw" "vulkan"))))
  (compile-c config compile?)

  (install config install?)

  (clean config clean?))
