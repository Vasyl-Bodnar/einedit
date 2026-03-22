#! /usr/local/bin/guile -s
!#

(add-to-load-path ".")
(use-modules (buildlib))

(define release? (memq #t (map (lambda (x) (equal? "release" x)) (command-line))))
(define install? (memq #t (map (lambda (x) (equal? "install" x)) (command-line))))
(define clean? (memq #t (map (lambda (x) (equal? "clean" x)) (command-line))))
(define compile? (not clean?))

;; Assumes that you already have glfw and vulkan installed
(let ((config
       (if release?
           (configure #:exe-name "deity"
                      #:link '("glfw" "vulkan")
                      #:optimization "-O3" #:debug "" #:derive '(NDEBUG))
           (configure #:exe-name "deity"
                      #:link '("glfw" "vulkan")))))
  (compile-c config compile?)

  (install config install?)

  (clean config clean?))
