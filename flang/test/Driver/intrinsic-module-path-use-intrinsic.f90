! Ensure that -fintrinsic-modules-path is also searched for modules named in
! USE, INTRINSIC, ahead of the default location, as it is for plain USE.

! RUN: %flang_fc1 -fsyntax-only %s 2>&1 | FileCheck %s --allow-empty --check-prefix=WITHOUT
! RUN: not %flang_fc1 -fsyntax-only -fintrinsic-modules-path %S/Inputs/ %s 2>&1 | FileCheck %s --check-prefix=GIVEN

! WITHOUT-NOT: 'ieee_arithmetic.mod' was not found
! WITHOUT-NOT: 'iso_fortran_env.mod' was not found

! GIVEN: error: Cannot use module file for module 'ieee_arithmetic': File has invalid checksum
! GIVEN: error: Cannot use module file for module 'iso_fortran_env': File has invalid checksum

program test_intrinsic_module_path_use_intrinsic
   use, intrinsic :: ieee_arithmetic, only: ieee_round_type
   use, intrinsic :: iso_fortran_env, only: team_type, event_type, lock_type
end program
