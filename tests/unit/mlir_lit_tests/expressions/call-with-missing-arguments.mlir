//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: not %revngcliftopt %s 2>&1 | FileCheck %s

!void = !clift.primitive<VoidKind 0>
!int32_t = !clift.primitive<SignedKind 4>

!f = !clift.defined<#clift.function<id = 1,
                                    name = "",
                                    return_type = !void,
                                    argument_types = [!int32_t]>>

%f = clift.undef : !f

// CHECK: argument count must match the number of function parameters
clift.call %f() : !f as ()
