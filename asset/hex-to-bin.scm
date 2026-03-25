#! /usr/local/bin/guile -s
!#
;; This Source Code Form is subject to the terms of the Mozilla Public
;; License, v. 2.0. If a copy of the MPL was not distributed with this
;; file, You can obtain one at http://mozilla.org/MPL/2.0/.

(use-modules (ice-9 textual-ports))
(use-modules (ice-9 binary-ports))
(use-modules (rnrs bytevectors))

(define filename (cadr (command-line)))

(define (row-type->bytes row-type)
  (case row-type
    ((wide)   32)
    ((normal) 16)
    ((short)  8)))

;; Twice the real one
(define (count->row-type count)
  (case count
    ((64) 'wide)
    ((32) 'normal)
    ((16) 'short)))

(define (row-type->hinibble row-type)
  (case row-type
    ((wide)   #x10000000)
    ((normal) #x00000000)
    ((short)  #x20000000)))

(define (put-nums-bv bv tmp-bv nums)
  (do ((nums nums (cdr nums))
       (idx 0 (+ idx 4)))
      ((null? nums) (put-bytevector bv tmp-bv 0 idx))
    (bytevector-u32-set! tmp-bv idx (car nums) (endianness little))))

;; Simple format
;; HEXFONT0
;; (repeat (4 byte range-start, where highest nibble is row-type, inclusive)
;;         (4 byte range-end, exclusive))
;; (00000030 ends the ranges with invalid 30 byte)
;; (00000000 as padding)
;; (repeat bitmaps of a 8, 16, or 32 byte value depending on row-type per range
;;         the number of bitmaps is then simply (range-end - range-start))
(call-with-input-file (string-append filename ".hex")
  (lambda (hex)
    (call-with-output-file (string-append filename ".bin")
      (lambda (bin)
        (put-string bin "HEXFONT0")
        (let* ((number-bv (make-bytevector 32))
               (content-bv
                (call-with-output-bytevector
                 (lambda (content-port)
                   (do ((line (get-line hex) (get-line hex))
                        (code-point 0)
                        (row-type 'normal)
                        (range-start 0) (range-end 0 code-point)
                        (offset 0))
                       ((eof-object? line)
                        (put-nums-bv bin number-bv
                                     (list (logior (row-type->hinibble row-type) range-start)
                                           range-end
                                           offset)))
                     (let* ((halves (string-split line #\:))
                            (new-code-point (string->number (car halves) 16))
                            (new-row-type (count->row-type (string-length (cadr halves))))
                            (bitmap-number (string->number (cadr halves) 16)))
                       (bytevector-uint-set! number-bv 0 bitmap-number (endianness little) (row-type->bytes new-row-type))
                       (put-bytevector content-port number-bv 0 (row-type->bytes new-row-type))
                       (unless (and (= new-code-point code-point)
                                    (eq? new-row-type row-type))
                         ;; Header is built here
                         (put-nums-bv bin number-bv
                                      (list (logior (row-type->hinibble row-type) range-start)
                                            range-end
                                            offset))
                         (set! range-start new-code-point)
                         (set! row-type new-row-type))
                       (set! code-point (1+ new-code-point))
                       (set! offset (+ (row-type->bytes new-row-type) offset))))))))
          ;; The header ends with (little-endian) value #x00000030, which is invalid for any range-start value
          (put-nums-bv bin number-bv '(#x30000000 #x00000000 #x00000000))
          (put-bytevector bin content-bv))))))
