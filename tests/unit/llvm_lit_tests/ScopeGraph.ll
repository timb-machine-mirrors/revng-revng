;
; This file is distributed under the MIT License. See LICENSE.md for details.
;

; RUN: %revngopt -load tests/unit/libtest_scopegraph.so %s -scope-graph-logger -o /dev/null |& FileCheck %s

; function tags metadata needed for all the tests
declare !revng.tags !0 void @scope-closer(ptr)
declare !revng.tags !1 void @goto-block()
!0 = !{!"scope-closer"}
!1 = !{!"goto-block"}

; no dashed edge test

define void @f() {
block_a:
  br i1 undef, label %block_b, label %block_c

block_b:
  ret void

block_c:
  br i1 undef, label %block_b, label %block_e

block_e:
  ret void
}

; CHECK-LABEL: ScopeGraph of function: f
; CHECK-NEXT: Block block_a successors:
; CHECK-NEXT:   block_b
; CHECK-NEXT:   block_c
; CHECK-NEXT: Block block_b successors:
; CHECK-NEXT: Block block_c successors:
; CHECK-NEXT:   block_b
; CHECK-NEXT:   block_e
; CHECK-NEXT: Block block_e successors:
; CHECK-NEXT: Depth first order:
; CHECK-NEXT: block_a
; CHECK-NEXT: block_b
; CHECK-NEXT: block_c
; CHECK-NEXT: block_e

; scope edge test, scope closer b->b is seen in the scopegraph

define void @g() {
block_a:
  br i1 undef, label %block_b, label %block_c

block_b:
  call void @scope-closer(ptr blockaddress(@g, %block_b))
  ret void

block_c:
  br i1 undef, label %block_b, label %block_e

block_e:
  ret void
}

; CHECK-LABEL: ScopeGraph of function: g
; CHECK-NEXT: Block block_a successors:
; CHECK-NEXT:   block_b
; CHECK-NEXT:   block_c
; CHECK-NEXT: Block block_b successors:
; CHECK-NEXT:   block_b
; CHECK-NEXT: Block block_c successors:
; CHECK-NEXT:   block_b
; CHECK-NEXT:   block_e
; CHECK-NEXT: Block block_e successors:
; CHECK-NEXT: Depth first order:
; CHECK-NEXT: block_a
; CHECK-NEXT: block_b
; CHECK-NEXT: block_c
; CHECK-NEXT: block_e

; goto edge test, b->c is not seen in the scopegraph

define void @h() {
block_a:
  br i1 undef, label %block_b, label %block_c

block_b:
  call void @goto-block()
  br label %block_c

block_c:
  br i1 undef, label %block_b, label %block_e

block_e:
  ret void
}

; CHECK-LABEL: ScopeGraph of function: h
; CHECK-NEXT: Block block_a successors:
; CHECK-NEXT:   block_b
; CHECK-NEXT:   block_c
; CHECK-NEXT: Block block_b successors:
; CHECK-NEXT: Block block_c successors:
; CHECK-NEXT:   block_b
; CHECK-NEXT:   block_e
; CHECK-NEXT: Block block_e successors:
; CHECK-NEXT: Depth first order:
; CHECK-NEXT: block_a
; CHECK-NEXT: block_b
; CHECK-NEXT: block_c
; CHECK-NEXT: block_e

; goto edge test, edge b->c is not seen in the scopegraph, but scope closer b->b
; is correctly seen

define void @i() {
block_a:
  br i1 undef, label %block_b, label %block_c

block_b:
  call void @goto-block()
  call void @scope-closer(ptr blockaddress(@i, %block_b))
  br label %block_c

block_c:
  br i1 undef, label %block_b, label %block_e

block_e:
  ret void
}

; CHECK-LABEL: ScopeGraph of function: i
; CHECK-NEXT: Block block_a successors:
; CHECK-NEXT:   block_b
; CHECK-NEXT:   block_c
; CHECK-NEXT: Block block_b successors:
; CHECK-NEXT:   block_b
; CHECK-NEXT: Block block_c successors:
; CHECK-NEXT:   block_b
; CHECK-NEXT:   block_e
; CHECK-NEXT: Block block_e successors:
; CHECK-NEXT: Depth first order:
; CHECK-NEXT: block_a
; CHECK-NEXT: block_b
; CHECK-NEXT: block_c
; CHECK-NEXT: block_e

; depth first test with different order on scopegraph wrt. cfg. The plain DFS on
; the cfg, would lead to a,b,c,e, while on the scopegraph we obtain a,b,e,c.

define void @l() {
block_a:
  br i1 undef, label %block_b, label %block_c

block_b:
  call void @scope-closer(ptr blockaddress(@l, %block_e))
  ret void

block_c:
  br i1 undef, label %block_b, label %block_e

block_e:
  ret void
}

; CHECK-LABEL: ScopeGraph of function: l
; CHECK-NEXT: Block block_a successors:
; CHECK-NEXT:   block_b
; CHECK-NEXT:   block_c
; CHECK-NEXT: Block block_b successors:
; CHECK-NEXT:   block_e
; CHECK-NEXT: Block block_c successors:
; CHECK-NEXT:   block_b
; CHECK-NEXT:   block_e
; CHECK-NEXT: Block block_e successors:
; CHECK-NEXT: Depth first order:
; CHECK-NEXT: block_a
; CHECK-NEXT: block_b
; CHECK-NEXT: block_e
; CHECK-NEXT: block_c