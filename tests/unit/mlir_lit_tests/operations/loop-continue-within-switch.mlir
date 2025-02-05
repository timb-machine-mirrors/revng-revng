//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: %revngcliftopt %s

!void = !clift.primitive<VoidKind 0>
!int32_t = !clift.primitive<SignedKind 4>

!f = !clift.defined<#clift.function<
  id = 1000,
  name = "f",
  return_type = !void,
  argument_types = []>>

clift.module {
  clift.func @f<!f>() {
    clift.while {
      %0 = clift.undef : !int32_t
      clift.yield %0 : !int32_t
    } {
      clift.switch {
        %0 = clift.undef : !int32_t
        clift.yield %0 : !int32_t
      } default {
        clift.loop_continue
      }
    }
  }
}
