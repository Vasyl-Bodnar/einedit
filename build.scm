#! /usr/local/bin/guile -s
!#

(add-to-load-path ".")
(use-modules (buildlib))

(define (check-arg name) (memq #t (map (lambda (x) (equal? name x)) (command-line))))
(define windows? (check-arg "windows"))
(define release? (check-arg "release"))
(define install? (check-arg "install"))
(define clean? (check-arg "clean"))
(define compile? (not clean?))
(define external? (check-arg "external"))
(define shader? (or external? (check-arg "shader")))
(define font? (or external? (check-arg "font")))

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
  (run-external config shader? #:name "glslc" #:args '("src/shader/basic.vert" "-o" "vert.spv") #:outputs '("vert.spv"))
  (run-external config shader? #:name "glslc" #:args '("src/shader/basic.frag" "-o" "frag.spv") #:outputs '("frag.spv"))
  (run-external config font? #:name "asset/hex-to-bin.scm" #:args '("asset/unscii-8") #:outputs '("asset/unscii-8.bin"))
  (run-external config font? #:name "asset/hex-to-bin.scm" #:args '("asset/unscii-16") #:outputs '("asset/unscii-16.bin"))

  (compile-c config compile?)

  (install config install?)

  (clean config clean?))
