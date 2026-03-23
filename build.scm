#! /usr/local/bin/guile -s
!#

(add-to-load-path ".")
(use-modules (buildlib))

(define windows? (memq #t (map (lambda (x) (equal? "windows" x)) (command-line))))
(define release? (memq #t (map (lambda (x) (equal? "release" x)) (command-line))))
(define install? (memq #t (map (lambda (x) (equal? "install" x)) (command-line))))
(define clean? (memq #t (map (lambda (x) (equal? "clean" x)) (command-line))))
(define compile? (not clean?))

;; Assumes that you already have glfw and vulkan installed
;; TODO: Currently the windows part needs manual link-path and include
;; I excluded them from this repo
(let ((config
       (if windows?
           (if release?
               (configure #:c-compiler "x86_64-w64-mingw32-gcc" #:exe-name "einedit"
                          #:link '("glfw3" "vulkan-1" "gdi32" "user32")
                          #:link-path '()
                          #:include '()
                          #:optimization "-O3" #:debug "" #:derive '(NDEBUG))
               (configure #:c-compiler "x86_64-w64-mingw32-gcc" #:exe-name "einedit"
                          #:link '("glfw3" "vulkan-1" "gdi32" "user32")
                          #:link-path '()
                          #:include '()))
           (if release?
               (configure #:exe-name "einedit"
                          #:link '("glfw" "vulkan")
                          #:optimization "-O3" #:debug "" #:derive '(NDEBUG))
               (configure #:exe-name "einedit"
                          #:link '("glfw" "vulkan"))))))
  (compile-c config compile?)

  (install config install?)

  (clean config clean?))
