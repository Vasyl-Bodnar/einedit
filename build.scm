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

;; Windows may require specified path
(define glfw-win-path (getenv "GLFW_WINDOWS_PATH"))
(define vulkan-win-path (getenv "VULKAN_WINDOWS_PATH"))

;; Assumes that you already have glfw and vulkan installed
(let ((config
       (if windows?
           (configure #:c-compiler "x86_64-w64-mingw32-gcc" #:exe-name "einedit"
                      #:link '((static "glfw3") "vulkan-1" (static "gdi32") (static "user32"))
                      #:link-path (list (in-vicinity glfw-win-path "lib-mingw-w64") (in-vicinity vulkan-win-path "Lib32"))
                      #:include (list (in-vicinity glfw-win-path "include") (in-vicinity vulkan-win-path "Include"))
                      #:optimization (if release? "-O3" "-O0") #:debug (if release? "" "-g") #:strip release? #:derive (if release? '(NDEBUG) '())
                      #:extra-args (if release? '("-Wl,-subsystem,windows") '()))
           (configure #:exe-name "einedit"
                      #:link '("glfw" "vulkan")
                      #:optimization (if release? "-O3" "-O0") #:debug (if release? "" "-g") #:strip release? #:derive (if release? '(NDEBUG) '())))))

  (run-external config shader? #:name "glslc" #:args '("src/shader/base.comp" "-o" "comp.spv") #:outputs '("comp.spv"))

  (run-external config font? #:name "asset/hex-to-bin.scm" #:args '("asset/unscii-8") #:outputs '("asset/unscii-8.bin"))
  (run-external config font? #:name "asset/hex-to-bin.scm" #:args '("asset/unscii-16") #:outputs '("asset/unscii-16.bin"))

  (compile-c config compile?)

  (install config install?)

  (clean config clean?))
